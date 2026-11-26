/* RetroAchievements over HTTPS on Linux (handhelds and desktops), included by ra_client.c when SS_RA_ONLINE is set.
 * The same approach as ReKHCoM's Linux handhelds (port/achievements/ra_https_linux.inc, MatyMeatBoy).
 *
 * OpenSSL is opened at run time: libssl.so.3 (the GA36 package carries it in libarmhf/ with libcrypto.so.3), else a
 * system's libssl.so.1.1 (Ubuntu 20.04 based firmwares such as ArkOS on the R36S). No headers are needed to build
 * and a system without it simply stays offline (the unlocks wait in retroachievements_pending.txt).
 * The server's certificate is always verified, against a CA bundle (SS_RA_CA_FILE, else achievements/cacert.pem in
 * the achievements folder, else the system's) and its name checked; there is no way to turn verification off.
 * Only the RetroAchievements hosts are contacted, as on Windows. HTTP/1.0 keeps the answer free of chunked encoding.
 * Nothing of a request (it holds the account's token) is written anywhere. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct RaSsl {
    int ready;
    void* (*TLS_client_method)(void);
    void* (*SSL_CTX_new)(const void*);
    void (*SSL_CTX_free)(void*);
    int (*SSL_CTX_load_verify_locations)(void*, const char*, const char*);
    int (*SSL_CTX_set_default_verify_paths)(void*);
    void (*SSL_CTX_set_verify)(void*, int, void*);
    void* (*SSL_new)(void*);
    void (*SSL_free)(void*);
    int (*SSL_set_fd)(void*, int);
    long (*SSL_ctrl)(void*, int, long, void*);
    int (*SSL_set1_host)(void*, const char*);
    int (*SSL_connect)(void*);
    int (*SSL_read)(void*, void*, int);
    int (*SSL_write)(void*, const void*, int);
    int (*SSL_shutdown)(void*);
    long (*SSL_get_verify_result)(const void*);
} RaSsl;

static RaSsl sRaSsl;
static SDL_InitState sRaSslInit;
static SDL_AtomicInt sRaSslTried;
static char sRaCaFile[1100];   /* <achievements folder>/achievements/cacert.pem, set by KhRaCreate */

static void RaSslLoad(void) {
    static const char* const names[] = { "libssl.so.3", "libssl.so.1.1", "libssl.so" };
    void* lib = NULL;
    size_t i;
    if (!SDL_ShouldInit(&sRaSslInit)) return;
    for (i = 0; !lib && i < sizeof names / sizeof names[0]; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
#define RA_SYM(name) (*(void**)&sRaSsl.name = lib ? dlsym(lib, #name) : NULL, sRaSsl.name != NULL)
    sRaSsl.ready = lib && RA_SYM(TLS_client_method) && RA_SYM(SSL_CTX_new) && RA_SYM(SSL_CTX_free) &&
        RA_SYM(SSL_CTX_load_verify_locations) && RA_SYM(SSL_CTX_set_default_verify_paths) && RA_SYM(SSL_CTX_set_verify) &&
        RA_SYM(SSL_new) && RA_SYM(SSL_free) && RA_SYM(SSL_set_fd) && RA_SYM(SSL_ctrl) && RA_SYM(SSL_set1_host) &&
        RA_SYM(SSL_connect) && RA_SYM(SSL_read) && RA_SYM(SSL_write) && RA_SYM(SSL_shutdown) &&
        RA_SYM(SSL_get_verify_result);
#undef RA_SYM
    SDL_SetAtomicInt(&sRaSslTried, 1);
    SDL_SetInitialized(&sRaSslInit, true);
}

/* 1 when OpenSSL was looked for and could not be used (said in the status file) */
static int RaSslMissing(void) { return SDL_GetAtomicInt(&sRaSslTried) && !sRaSsl.ready; }

/* a socket to host:port within a few seconds, or -1 */
static int RaConnect(const char* host, const char* port) {
    struct addrinfo hints, *list = NULL, *a;
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &list) != 0) return -1;
    for (a = list; a && fd < 0; a = a->ai_next) {
        struct pollfd p;
        struct timeval tv = {10, 0};
        int flags, error = 0;
        socklen_t length = sizeof(error);
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        p.fd = fd; p.events = POLLOUT; p.revents = 0;
        if (poll(&p, 1, 8000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error) {
            close(fd); fd = -1; continue;
        }
        fcntl(fd, F_SETFL, flags);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    freeaddrinfo(list);
    return fd;
}

static int RaLoadCertificates(void* ctx) {
    static const char* const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem",
        "/etc/ssl/ca-bundle.pem" };
    const char* own = getenv("SS_RA_CA_FILE");
    size_t i;
    if (own && own[0] && access(own, R_OK) == 0 && sRaSsl.SSL_CTX_load_verify_locations(ctx, own, NULL) == 1) return 1;
    if (sRaCaFile[0] && access(sRaCaFile, R_OK) == 0 && sRaSsl.SSL_CTX_load_verify_locations(ctx, sRaCaFile, NULL) == 1)
        return 1;
    for (i = 0; i < sizeof(bundles) / sizeof(bundles[0]); i++)
        if (access(bundles[i], R_OK) == 0 && sRaSsl.SSL_CTX_load_verify_locations(ctx, bundles[i], NULL) == 1)
            return 1;
    return sRaSsl.SSL_CTX_set_default_verify_paths(ctx) == 1;
}

/* One request: fills job->status and job->body (status <= 0: rcheevos' retryable client error). */
static void RaHttpsPosix(KhRaJob* job) {
    char host[256], port[8] = "443", *request = NULL;
    const char *url = job->url, *rest, *path, *colon;
    void *ctx = NULL, *ssl = NULL;
    size_t hostLength, postLength = job->post ? strlen(job->post) : 0, requestLength = 0, size = 0, capacity = 0;
    int fd = -1, n;
    char* body = NULL;
    job->status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    if (!url || strncmp(url, "https://", 8) != 0) return;
    rest = url + 8;
    path = strchr(rest, '/');
    if (!path) path = rest + strlen(rest);
    colon = memchr(rest, ':', (size_t)(path - rest));
    hostLength = (size_t)((colon ? colon : path) - rest);
    if (!hostLength || hostLength >= sizeof(host)) return;
    memcpy(host, rest, hostLength);
    host[hostLength] = 0;
    if (strcasecmp(host, "retroachievements.org") != 0 && strcasecmp(host, "media.retroachievements.org") != 0 &&
        strcasecmp(host, "static.retroachievements.org") != 0) return;
    if (colon) snprintf(port, sizeof(port), "%.*s", (int)(path - colon - 1), colon + 1);
    if (!*path) path = "/";
    RaSslLoad();
    if (!sRaSsl.ready) return;
    requestLength = strlen(path) + strlen(host) + postLength + 512;
    request = (char*)malloc(requestLength);
    if (!request) return;
    n = snprintf(request, requestLength,
                 "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ShiningSource/0.1 rcheevos/12.5\r\nAccept: */*\r\n"
                 "%s%s%sContent-Length: %u\r\nConnection: close\r\n\r\n",
                 job->post ? "POST" : "GET", path, host,
                 job->content_type ? "Content-Type: " : "", job->content_type ? job->content_type : "",
                 job->content_type ? "\r\n" : "", (unsigned)postLength);
    if (n < 0 || (size_t)n + postLength >= requestLength) goto done;
    if (postLength) memcpy(request + n, job->post, postLength);
    requestLength = (size_t)n + postLength;
    if ((fd = RaConnect(host, port)) < 0) goto done;
    if (!(ctx = sRaSsl.SSL_CTX_new(sRaSsl.TLS_client_method())) || !RaLoadCertificates(ctx)) goto done;
    sRaSsl.SSL_CTX_set_verify(ctx, 1 /* SSL_VERIFY_PEER */, NULL);
    if (!(ssl = sRaSsl.SSL_new(ctx)) || sRaSsl.SSL_set_fd(ssl, fd) != 1 ||
        sRaSsl.SSL_ctrl(ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0 /* host_name */, host) != 1 ||
        sRaSsl.SSL_set1_host(ssl, host) != 1 || sRaSsl.SSL_connect(ssl) != 1 ||
        sRaSsl.SSL_get_verify_result(ssl) != 0 /* X509_V_OK */)
        goto done;
    for (size_t sent = 0; sent < requestLength; ) {
        int w = sRaSsl.SSL_write(ssl, request + sent, (int)SDL_min(requestLength - sent, 16384));
        if (w <= 0) goto done;
        sent += (size_t)w;
    }
    for (;;) {
        if (size + 4096 + 1 > capacity) {
            char* grown;
            if (capacity >= 8u << 20) goto done; /* an answer this large is not one of ours */
            capacity = capacity ? capacity * 2 : 16384;
            if (!(grown = (char*)realloc(body, capacity))) goto done;
            body = grown;
        }
        n = sRaSsl.SSL_read(ssl, body + size, 4096);
        if (n <= 0) break;
        size += (size_t)n;
    }
    if (body && size > 12 && !strncmp(body, "HTTP/1.", 7)) {
        char* end;
        body[size] = 0;
        end = strstr(body, "\r\n\r\n");
        if (end) {
            int status = atoi(body + 9);
            size_t header = (size_t)(end + 4 - body);
            job->body_size = size - header;
            job->body = (char*)malloc(job->body_size + 1);
            if (job->body) {
                memcpy(job->body, end + 4, job->body_size);
                job->body[job->body_size] = 0;
                job->status = status;
            } else job->body_size = 0;
        }
    }
done:
    if (ssl) { sRaSsl.SSL_shutdown(ssl); sRaSsl.SSL_free(ssl); }
    if (ctx) sRaSsl.SSL_CTX_free(ctx);
    if (fd >= 0) close(fd);
    if (body) { SecureZeroMemory(body, capacity); free(body); }
    if (request) { SecureZeroMemory(request, requestLength > 0 ? requestLength : 1); free(request); }
}
