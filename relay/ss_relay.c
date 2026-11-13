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
 * SS_RELAY_OUTAGE=player:start_ms:length_ms[,...] cuts one player off for a while, as a connection that is gone
 * and comes back: from start_ms after that seat (1 to 4) was first taken, for length_ms, everything from that
 * player's address is thrown away unread and nothing is sent to it. The relay then behaves as it does for any
 * silent player (its seat is freed after 30 seconds and given back when it says HELLO again).
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

/* SS_RELAY_OUTAGE: per seat up to OUTAGES windows, counted from the moment the seat was first taken; the address
 * that took it is the player that is cut off (its seat may be freed in between). */
enum { OUTAGES = 4 };
struct outage {
    uint32_t start, length;
};
static struct outage outages[SEATS][OUTAGES];
static int outage_any, outage_seated[SEATS], outage_now[SEATS];
static uint32_t outage_origin[SEATS];
static struct sockaddr_in outage_address[SEATS];

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

static int cut_off(const struct sockaddr_in *address);

static void forward(socket_t sock, const uint8_t *packet, int size, const struct sockaddr_in *to)
{
    forwarded++;
    if (cut_off(to))
        return;
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

static int same_address(const struct sockaddr_in *a, const struct sockaddr_in *b);

/* Is that address cut off right now (SS_RELAY_OUTAGE)? Says so on the console when a window opens or closes. */
static int cut_off(const struct sockaddr_in *address)
{
    uint32_t now = ticks();
    int seat, i, cut = 0;
    if (!outage_any)
        return 0;
    for (seat = 0; seat < SEATS; seat++) {
        int inside = 0;
        if (!outage_seated[seat])
            continue;
        for (i = 0; i < OUTAGES; i++)
            if (outages[seat][i].length && now - outage_origin[seat] >= outages[seat][i].start
                && now - outage_origin[seat] - outages[seat][i].start < outages[seat][i].length)
                inside = 1;
        if (inside != outage_now[seat]) {
            outage_now[seat] = inside;
            /* the clock is the one the players' test logs use on the same machine */
            fprintf(stderr, "outage: seat %d %s at %u ms (clock %u)\n", seat + 1, inside ? "cut off" : "back",
                    (unsigned)(now - outage_origin[seat]), (unsigned)now);
            fflush(stderr);
        }
        if (inside && same_address(&outage_address[seat], address))
            cut = 1;
    }
    return cut;
}

static void release_held(socket_t sock)
{
    uint32_t now = ticks();
    int i = 0;
    while (i < held_count) {
        if ((int32_t)(now - held[i].due) >= 0) {
            if (!cut_off(&held[i].to))
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

static int idle_exit;
static time_t last_packet;

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
    idle_exit = getenv("SS_RELAY_IDLE_EXIT") ? atoi(getenv("SS_RELAY_IDLE_EXIT")) : 0;
    sim_loss = getenv("SS_RELAY_LOSS") ? atoi(getenv("SS_RELAY_LOSS")) : 0;
    sim_lag = getenv("SS_RELAY_LAG") ? atoi(getenv("SS_RELAY_LAG")) : 0;
    sim_jitter = getenv("SS_RELAY_JITTER") ? atoi(getenv("SS_RELAY_JITTER")) : 0;
    if (sim_lag || sim_jitter)
        held = malloc(sizeof *held * HELD);
    if (getenv("SS_RELAY_OUTAGE")) {
        const char *text = getenv("SS_RELAY_OUTAGE");
        while (*text) {
            char *end;
            unsigned long player = strtoul(text, &end, 10), start = 0, length = 0;
            int i;
            if (*end == ':')
                start = strtoul(end + 1, &end, 10);
            if (*end == ':')
                length = strtoul(end + 1, &end, 10);
            for (i = 0; player >= 1 && player <= SEATS && length && i < OUTAGES; i++)
                if (!outages[player - 1][i].length) {
                    outages[player - 1][i].start = (uint32_t)start;
                    outages[player - 1][i].length = (uint32_t)length;
                    outage_any = 1;
                    fprintf(stderr, "outage: seat %lu will be cut off from %lu ms after it joined, for %lu ms\n", player, start, length);
                    break;
                }
            text = *end == ',' ? end + 1 : "";
        }
    }
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
        /* SS_RELAY_IDLE_EXIT=seconds: a relay a player started on their own PC ends by itself when nobody has
         * sent it anything for that long, so it is not left running for ever after the game. */
        if (size > 0 || !last_packet)
            last_packet = now;
        if (idle_exit && now - last_packet > idle_exit) {
            fprintf(stderr, "no packet for %d seconds: leaving\n", idle_exit);
            return 0;
        }
        if (now != swept) {
            swept = now;
            for (i = 0; i < ROOMS; i++)
                for (j = 0; rooms[i].players && j < SEATS; j++)
                    if (rooms[i].seat[j].used && now - rooms[i].seat[j].heard > SILENCE)
                        release(&rooms[i], j, "went silent");
        }
        if (size < HEADER || size > MAX_PACKET || rd32(packet) != MAGIC)
            continue;
        if (cut_off(&from))
            continue;                               /* SS_RELAY_OUTAGE: this player's connection is "gone" */
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
                if (outage_any && !outage_seated[seat]) {
                    outage_seated[seat] = 1;
                    outage_origin[seat] = ticks();
                    outage_address[seat] = from;
                }
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
