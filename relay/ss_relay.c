/* ShiningSource relay: forwards the players' packets inside a room. One process, one UDP port.
 *
 *   ss_relay [port]            default 47900
 *
 * The relay knows nothing about the game. A player says HELLO with a room name and gets a seat (1 to 4, the
 * first one is the host; a fifth is not answered); after that every packet it sends is passed to the other seats of its room, stamped
 * with its seat. A seat is freed when its player says it left or stays silent for 30 seconds; a player that
 * comes back with the same token gets its seat back if it is still free.
 *
 * Packet layout (shared with runtime/core/netlink.c): u32 magic "SSN1", u8 type, u8 seat, u8 delay, u8 count,
 * u32 room, u32 token, ... The relay reads only those fields.
 *
 * Testing a bad network on one machine: SS_RELAY_LOSS=percent of forwarded packets dropped, SS_RELAY_LAG=ms each
 * packet is held, SS_RELAY_JITTER=ms of extra random hold (packets then also arrive out of order).
 *
 * Build: cc -O2 -o ss_relay ss_relay.c        (Windows: add -lws2_32)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int socklen_t_;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
typedef socklen_t socklen_t_;
#endif

enum { SEATS = 4, ROOMS = 512, HEADER = 60, MAX_PACKET = 1200, SILENCE = 30 };
enum { T_HELLO = 1, T_WELCOME, T_DATA, T_END, T_PING };
#define MAGIC 0x314E5353u

struct seat {
    int used;
    uint32_t token;
    struct sockaddr_in address;
    time_t heard;
};
struct room {
    uint32_t id;
    int players;
    struct seat seat[SEATS];
};

static struct room rooms[ROOMS];
static unsigned long forwarded;

/* ---- bad-network simulation (off unless asked for) ---- */
enum { HELD = 8192 };
struct held {
    uint32_t due;
    int size;
    struct sockaddr_in to;
    uint8_t data[MAX_PACKET];
};
static struct held *held;
static int held_count, sim_loss, sim_lag, sim_jitter;

static uint32_t ticks(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000u + now.tv_nsec / 1000000u);
#endif
}

static void forward(socket_t sock, const uint8_t *packet, int size, const struct sockaddr_in *to)
{
    forwarded++;
    if (sim_loss && rand() % 100 < sim_loss)
        return;
    if ((sim_lag || sim_jitter) && held && held_count < HELD) {
        struct held *h = &held[held_count++];
        h->due = ticks() + (uint32_t)sim_lag + (uint32_t)(sim_jitter ? rand() % (sim_jitter + 1) : 0);
        h->size = size;
        h->to = *to;
        memcpy(h->data, packet, (size_t)size);
        return;
    }
    sendto(sock, (const char *)packet, size, 0, (const struct sockaddr *)to, sizeof *to);
}

static void release_held(socket_t sock)
{
    uint32_t now = ticks();
    int i = 0;
    while (i < held_count) {
        if ((int32_t)(now - held[i].due) >= 0) {
            sendto(sock, (const char *)held[i].data, held[i].size, 0, (const struct sockaddr *)&held[i].to, sizeof held[i].to);
            held[i] = held[--held_count];
        }
        else
            i++;
    }
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void note(const char *what, const struct room *room, int seat)
{
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", gmtime(&now));
    fprintf(stderr, "%s room %08X seat %d %s (%d in room, %lu packets forwarded)\n", stamp, (unsigned)room->id, seat + 1, what,
            room->players, forwarded);
    fflush(stderr);
}

static void release(struct room *room, int seat, const char *why)
{
    if (!room->seat[seat].used)
        return;
    room->seat[seat].used = 0;
    room->players--;
    note(why, room, seat);
}

static struct room *find_room(uint32_t id, int create)
{
    struct room *spare = NULL;
    int i;
    for (i = 0; i < ROOMS; i++) {
        if (rooms[i].players && rooms[i].id == id)
            return &rooms[i];
        if (!rooms[i].players && !spare)
            spare = &rooms[i];
    }
    if (!create || !spare)
        return NULL;
    memset(spare, 0, sizeof *spare);
    spare->id = id;
    return spare;
}

static int same_address(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 47900;
    struct sockaddr_in local;
    socket_t sock;
    time_t swept = 0;
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    sim_loss = getenv("SS_RELAY_LOSS") ? atoi(getenv("SS_RELAY_LOSS")) : 0;
    sim_lag = getenv("SS_RELAY_LAG") ? atoi(getenv("SS_RELAY_LAG")) : 0;
    sim_jitter = getenv("SS_RELAY_JITTER") ? atoi(getenv("SS_RELAY_JITTER")) : 0;
    if (sim_lag || sim_jitter)
        held = malloc(sizeof *held * HELD);
    if (sim_loss || sim_lag || sim_jitter)
        fprintf(stderr, "simulating a bad network: %d%% loss, %d ms lag, %d ms jitter\n", sim_loss, sim_lag, sim_jitter);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((unsigned short)port);
    if (bind(sock, (struct sockaddr *)&local, sizeof local)) {
        fprintf(stderr, "cannot bind UDP port %d\n", port);
        return 1;
    }
    fprintf(stderr, "ss_relay on UDP port %d\n", port);
    for (;;) {
        uint8_t packet[MAX_PACKET + 1];
        struct sockaddr_in from;
        socklen_t_ from_size = sizeof from;
        struct timeval wait = { held ? 0 : 1, held ? 1000 : 0 };
        struct room *room;
        fd_set set;
        time_t now;
        int size = 0, i, j, seat;

        FD_ZERO(&set);
        FD_SET(sock, &set);
        if (select((int)sock + 1, &set, NULL, NULL, &wait) > 0)
            size = (int)recvfrom(sock, (char *)packet, sizeof packet, 0, (struct sockaddr *)&from, &from_size);
        if (held)
            release_held(sock);
        now = time(NULL);
        if (now != swept) {
            swept = now;
            for (i = 0; i < ROOMS; i++)
                for (j = 0; rooms[i].players && j < SEATS; j++)
                    if (rooms[i].seat[j].used && now - rooms[i].seat[j].heard > SILENCE)
                        release(&rooms[i], j, "went silent");
        }
        if (size < HEADER || size > MAX_PACKET || rd32(packet) != MAGIC)
            continue;
        if (packet[4] == T_PING) {                  /* echo: lets a player measure its way to the relay */
            sendto(sock, (const char *)packet, HEADER, 0, (struct sockaddr *)&from, sizeof from);
            continue;
        }
        room = find_room(rd32(packet + 8), packet[4] == T_HELLO);
        if (!room)
            continue;
        seat = -1;
        for (j = 0; j < SEATS; j++)
            if (room->seat[j].used && room->seat[j].token == rd32(packet + 12))
                seat = j;
        if (packet[4] == T_HELLO) {
            if (seat < 0) {
                if (packet[5] < SEATS && !room->seat[packet[5]].used)
                    seat = packet[5];               /* the seat it had before the relay or the player restarted */
                for (j = 0; seat < 0 && j < SEATS; j++)
                    if (!room->seat[j].used)
                        seat = j;
                if (seat < 0)
                    continue;                       /* room full */
                room->seat[seat].used = 1;
                room->seat[seat].token = rd32(packet + 12);
                room->players++;
                room->seat[seat].address = from;
                note("joined", room, seat);
            }
            room->seat[seat].address = from;        /* follows a player whose router changed the mapping */
            room->seat[seat].heard = now;
            packet[4] = T_WELCOME;
            packet[5] = (uint8_t)seat;
            packet[7] = 0;
            for (j = 0; j < SEATS; j++)
                if (room->seat[j].used)
                    packet[7] |= (uint8_t)(1 << j);
            sendto(sock, (const char *)packet, HEADER, 0, (struct sockaddr *)&from, sizeof from);
            continue;
        }
        if (seat < 0 || !same_address(&room->seat[seat].address, &from) || (packet[4] != T_DATA && packet[4] != T_END))
            continue;
        room->seat[seat].heard = now;
        packet[5] = (uint8_t)seat;
        for (j = 0; j < SEATS; j++)
            if (j != seat && room->seat[j].used) {
                forward(sock, packet, size, &room->seat[j].address);
            }
        if (packet[4] == T_END)
            release(room, seat, "left");
    }
}
