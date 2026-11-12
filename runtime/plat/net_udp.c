/* One UDP socket to the relay, for every desktop platform. The core (core/netlink.c) decides what travels. */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_t;
#define BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int socket_t;
#define BAD_SOCKET (-1)
#define closesocket close
#endif

static socket_t sock = BAD_SOCKET;
static unsigned char farewell[128];
static int farewell_size, farewell_hooked;

static void say_farewell(void)
{
    if (sock != BAD_SOCKET && farewell_size > 0) {
        send(sock, (const char *)farewell, farewell_size, 0);
        send(sock, (const char *)farewell, farewell_size, 0);
    }
}

void ss_plat_net_farewell(const void *data, int size)
{
    if (size < 0 || size > (int)sizeof farewell)
        size = 0;
    if (size)
        memcpy(farewell, data, (size_t)size);
    farewell_size = size;
    if (!farewell_hooked) {
        farewell_hooked = 1;
        atexit(say_farewell);
    }
}

int ss_plat_net_open(const char *host, int port)
{
    struct addrinfo hints, *found = NULL, *it;
    char service[16];
#ifdef _WIN32
    WSADATA data;
    u_long on = 1;
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return -1;
#endif
    if (sock != BAD_SOCKET) {
        closesocket(sock);
        sock = BAD_SOCKET;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(host, service, &hints, &found) || !found)
        return -2;
    for (it = found; it; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == BAD_SOCKET)
            continue;
        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0)
            break;
        closesocket(sock);
        sock = BAD_SOCKET;
    }
    freeaddrinfo(found);
    if (sock == BAD_SOCKET)
        return -3;
#ifdef _WIN32
    ioctlsocket(sock, FIONBIO, &on);
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    return 0;
}

int ss_plat_net_send(const void *data, int size)
{
    if (sock == BAD_SOCKET)
        return -1;
    return (int)send(sock, (const char *)data, size, 0);
}

int ss_plat_net_recv(void *data, int capacity)
{
    int got;
    if (sock == BAD_SOCKET)
        return 0;
    got = (int)recv(sock, (char *)data, capacity, 0);
    return got > 0 ? got : 0;
}

void ss_plat_net_wait(int milliseconds)
{
    if (sock != BAD_SOCKET) {
        fd_set set;
        struct timeval timeout;
        FD_ZERO(&set);
        FD_SET(sock, &set);
        timeout.tv_sec = milliseconds / 1000;
        timeout.tv_usec = (milliseconds % 1000) * 1000;
        select((int)sock + 1, &set, NULL, NULL, &timeout);
    }
    else {
#ifdef _WIN32
        Sleep((DWORD)milliseconds);
#else
        usleep((useconds_t)milliseconds * 1000);
#endif
    }
    ss_plat_pump();
}

uint32_t ss_plat_ticks_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000u + now.tv_nsec / 1000000u);
#endif
}
