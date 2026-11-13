/* Online play: the link cable replaced by the network, under the game's own link driver.
 *
 * The game talks to the cable through the SDK's MultiSio library: once per frame it hands over one 24-byte
 * block and gets back the block each connected console sent, plus a status word. Its init and per-frame entry
 * points are replaced here (linked with --wrap); nothing above them changes, so the game's own lobby, handshakes and
 * packets run as they do over a cable.
 *
 * Rules (the GBANoCable model):
 *   - Every block carries a label on one shared timeline. The host (seat 0) owns the timeline; the others adopt
 *     it when they first hear the host.
 *   - At timeline frame F every player is given the blocks labelled F - delay of every player, itself included.
 *     All players therefore see exactly the same data at the same label, whatever the route or the latency.
 *   - A block that has not arrived is waited for, not skipped: the frame stalls. Whoever runs ahead waits, so
 *     the players settle at offsets the delay can cover. A player is given up only after a long silence, or
 *     when it says it left.
 *   - Lost packets cost nothing: every packet repeats all blocks the others have not acknowledged yet.
 *
 * The delay is as short as the connection allows. Unless SS_NET_DELAY fixes it, a session starts at two frames
 * (the cable's own is one) and the host raises it by one whenever a player reports that it keeps having to wait.
 * It is raised at a frame of the timeline announced in advance; for that one frame nobody is given new blocks,
 * which the game takes as it takes a noisy cable. It is never lowered during a session: that would skip blocks.
 *
 * Environment: SS_NET=host:port (the relay), SS_NET_ROOM=name, SS_NET_DELAY=frames (fixed; automatic if unset or 0),
 * SS_NET_TIMEOUT=milliseconds of silence before a player is dropped (default 15000), SS_NET_LOG=file (one line
 * per delivered frame: label, players, hash of the blocks; identical on every player), SS_NET_PLAYERS=n (tests:
 * wait at the start of the session until n players are in the room).
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO16(o) (*(volatile uint16_t *)(uintptr_t)(SS_IO + (o)))

enum { BLOCK = 24, SEATS = 4, RING = 256, MAX_BLOCKS = 40, HEADER = 60, FIRST_LABEL = 1000 };
enum { T_HELLO = 1, T_WELCOME, T_DATA, T_END };
#define MAGIC 0x314E5353u       /* "SSN1" */

void sub_0800170C(void);
uint32_t sub_08001804(void *sendp, void *recvp);
void __real_sub_0800170C(void);
uint32_t __real_sub_08001804(void *sendp, void *recvp);

struct stream {                 /* the blocks of one seat, by label */
    int known;                  /* a stream of this seat is being received */
    uint32_t epoch, first;      /* which stream, and its first label */
    uint32_t floor;             /* lowest label this player will be given: older ones were never needed */
    uint32_t next;              /* lowest label not received yet (everything below is here or already used) */
    uint32_t label[RING];       /* label + 1 of the block held in each ring slot, 0 if empty */
    uint8_t block[RING][BLOCK];
    uint32_t heard;             /* time of the last packet */
    uint32_t acked;             /* next label that seat expects of my stream */
    int vote;                   /* that player wants to skip the scene */
    int raise, raise_seen;      /* how often it asked for a longer delay, and how much of that the host has answered */
    uint32_t skip_at;           /* the host's: label at which everybody skips, 0 if none */
};

static int configured = -1, gather, seat = -1, synced, delay = 4, timeout_ms = 15000;
static uint32_t room, token, epoch, farewell_epoch, frame_label, own_first, connected, available, last_hello, last_send;
static struct stream streams[SEATS];
static void start_session(void);
static uint8_t own[RING][BLOCK];
static char room_name[32] = "default", status_line[96];
static FILE *log_file;
static int my_vote;
static int auto_delay = 1, stall_frames, raise_request, raise_seen_own, new_delay, quiet_windows;
static uint32_t change_at, change_done, last_target;
static uint32_t skip_at, skip_done;
enum { TRAILER = 12, TRAILER_MARK = 0xA5 };

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t fnv(uint32_t hash, const void *data, int size)
{
    const uint8_t *bytes = data;
    while (size-- > 0)
        hash = (hash ^ *bytes++) * 16777619u;
    return hash;
}

static int configure(void)
{
    const char *text = getenv("SS_NET");
    char host[128];
    int port = 47900, i;

    configured = 0;
    if (!text || !*text)
        return 0;
    for (i = 0; text[i] && text[i] != ':' && i < (int)sizeof host - 1; i++)
        host[i] = text[i];
    host[i] = 0;
    if (text[i] == ':')
        port = (int)strtoul(text + i + 1, NULL, 10);
    if (getenv("SS_NET_ROOM"))
        snprintf(room_name, sizeof room_name, "%s", getenv("SS_NET_ROOM"));
    delay = getenv("SS_NET_DELAY") ? (int)strtoul(getenv("SS_NET_DELAY"), NULL, 10) : 0;
    auto_delay = delay <= 0;
    if (auto_delay) delay = 2;
    if (delay > 30) delay = 30;
    if (getenv("SS_NET_TIMEOUT"))
        timeout_ms = (int)strtoul(getenv("SS_NET_TIMEOUT"), NULL, 10);
    if (getenv("SS_NET_PLAYERS"))
        gather = (int)strtoul(getenv("SS_NET_PLAYERS"), NULL, 10);
    if (getenv("SS_NET_LOG"))
        log_file = fopen(getenv("SS_NET_LOG"), "w");
    room = fnv(2166136261u, room_name, (int)strlen(room_name));
    {
        /* Players only meet if they run the same game and the same rules: the room is the name together with
         * the cartridge's contents and the version of this protocol. Another version of the port or another
         * ROM lands in another room instead of producing a session that drifts apart. */
        static const uint32_t rules = 2;        /* raise when the transport or anything the players must agree on changes */
        room = fnv(room, (const void *)(uintptr_t)SS_ROM, 0x1000000);
        room = fnv(room, &rules, sizeof rules);
    }
    token = getenv("SS_NET_TOKEN") ? (uint32_t)strtoul(getenv("SS_NET_TOKEN"), NULL, 10)
                                   : fnv(ss_plat_ticks_ms() * 2654435761u, &text, sizeof text) | 1;
    if (ss_plat_net_open(host, port)) {
        fprintf(stderr, "net: cannot reach %s:%d, playing offline\n", host, port);
        return 0;
    }
    fprintf(stderr, "net: relay %s:%d, room '%s', delay %d\n", host, port, room_name, delay);
    configured = 1;
    return 1;
}

static void header(uint8_t *packet, int type, int count, uint32_t first_label)
{
    int i;
    memset(packet, 0, HEADER);
    wr32(packet, MAGIC);
    packet[4] = (uint8_t)type;
    packet[5] = (uint8_t)seat;
    packet[6] = (uint8_t)delay;
    packet[7] = (uint8_t)count;
    wr32(packet + 8, room);
    wr32(packet + 12, token);
    wr32(packet + 16, epoch);
    wr32(packet + 20, own_first);
    wr32(packet + 24, first_label);
    for (i = 0; i < SEATS; i++) {
        wr32(packet + 28 + i * 4, streams[i].known ? streams[i].next : 0);
        wr32(packet + 44 + i * 4, streams[i].known ? streams[i].epoch : 0);
    }
}

static void send_hello(void)
{
    uint8_t packet[HEADER + 32];
    header(packet, T_HELLO, 0, 0);
    memset(packet + HEADER, 0, 32);
    memcpy(packet + HEADER, room_name, strlen(room_name));
    ss_plat_net_send(packet, sizeof packet);
    last_hello = ss_plat_ticks_ms();
}

/* Everything the others have not acknowledged, up to the block of the current frame. */
static void send_data(void)
{
    uint8_t packet[HEADER + MAX_BLOCKS * BLOCK + TRAILER];
    uint32_t from = frame_label, label;
    int i, count = 0;

    if (!synced)
        return;
    for (i = 0; i < SEATS; i++)
        if (i != seat && streams[i].known && streams[i].acked >= own_first && streams[i].acked < from)
            from = streams[i].acked;
    if (frame_label - from >= MAX_BLOCKS)
        from = frame_label - MAX_BLOCKS + 1;
    if (from < own_first)
        from = own_first;
    for (label = from; label <= frame_label; label++, count++)
        memcpy(packet + HEADER + count * BLOCK, own[label % RING], BLOCK);
    header(packet, T_DATA, count, from);
    {   /* after the blocks: this player's vote to skip the scene and, from the host, the frame to do it at */
        uint8_t *trailer = packet + HEADER + count * BLOCK;
        memset(trailer, 0, TRAILER);
        trailer[0] = TRAILER_MARK;
        trailer[1] = (uint8_t)my_vote;
        trailer[2] = (uint8_t)raise_request;
        trailer[3] = (uint8_t)(seat == 0 ? new_delay : 0);
        wr32(trailer + 8, seat == 0 ? change_at : 0);
        wr32(trailer + 4, seat == 0 ? skip_at : 0);
    }
    ss_plat_net_send(packet, HEADER + count * BLOCK + TRAILER);
    last_send = ss_plat_ticks_ms();
}

static void status(int waiting_for)
{
    char line[96] = { 0 };
    int i, players = synced ? 1 : 0;
    for (i = 0; i < SEATS; i++)
        players += i != seat && streams[i].known;
    if (seat < 0)
        snprintf(line, sizeof line, "online: looking for the server");
    else if (waiting_for >= 0)
        snprintf(line, sizeof line, "online: waiting for player %d", waiting_for + 1);
    else if (!synced)
        snprintf(line, sizeof line, "online '%s': player %d, waiting for the host", room_name, seat + 1);
    else
        snprintf(line, sizeof line, "online '%s': player %d of %d%s", room_name, seat + 1, players, seat == 0 ? " (host)" : "");
    if (memcmp(line, status_line, sizeof line)) {
        memcpy(status_line, line, sizeof line);
        ss_plat_status(line);
    }
}

static void drop(int who, const char *why)
{
    if (streams[who].known)
        fprintf(stderr, "net: player %d %s\n", who + 1, why);
    memset(&streams[who], 0, sizeof streams[who]);
    connected &= ~(1u << who);
    if (who == 0 && seat > 0)
        synced = 0;             /* the timeline is the host's */
}

static void receive(void)
{
    uint8_t packet[HEADER + MAX_BLOCKS * BLOCK + TRAILER + 64];
    int size, i;

    while ((size = ss_plat_net_recv(packet, sizeof packet)) >= HEADER) {
        int type = packet[4], from = packet[5], count = packet[7];
        struct stream *s;
        uint32_t first, label;

        if (rd32(packet) != MAGIC || rd32(packet + 8) != room)
            continue;
        if (type == T_WELCOME) {
            if (from < SEATS && seat != from) {
                seat = from;
                fprintf(stderr, "net: seat %d of room '%s'\n", seat + 1, room_name);
                start_session();
            }
            continue;
        }
        if (from >= SEATS || from == seat || seat < 0)
            continue;
        s = &streams[from];
        if (type == T_END) {
            if (s->known && s->epoch == rd32(packet + 16))
                drop(from, "left");
            continue;
        }
        if (type != T_DATA || size < HEADER + count * BLOCK)
            continue;
        if (!s->known || s->epoch != rd32(packet + 16)) {       /* a new stream of that seat */
            if (s->known)
                drop(from, "restarted");
            s->known = 1;
            s->epoch = rd32(packet + 16);
            s->first = rd32(packet + 20);
            s->floor = s->first;
            if (synced && frame_label - delay > s->floor)
                s->floor = frame_label - delay;                 /* it was here before me, or I before it */
            s->next = s->floor;
            fprintf(stderr, "net: player %d connected\n", from + 1);
        }
        s->heard = ss_plat_ticks_ms();
        if (rd32(packet + 44 + seat * 4) == epoch)
            s->acked = rd32(packet + 28 + seat * 4);
        if (size >= HEADER + count * BLOCK + TRAILER && packet[HEADER + count * BLOCK] == TRAILER_MARK) {
            const uint8_t *trailer = packet + HEADER + count * BLOCK;
            s->vote = trailer[1] != 0;
            s->raise = trailer[2];
            if (from == 0 && trailer[3] && rd32(trailer + 8) > change_done) {
                new_delay = trailer[3];
                change_at = rd32(trailer + 8);
            }
            if (from == 0 && rd32(trailer + 4) > skip_done)
                skip_at = rd32(trailer + 4);
        }
        first = rd32(packet + 24);
        if (from == 0 && !synced && first > s->next)
            s->next = first;            /* not on the timeline yet: any recent block of the host tells where it is */
        for (i = 0; i < count; i++) {
            label = first + (uint32_t)i;
            if (label < s->next || label - s->next >= RING)
                continue;
            memcpy(s->block[label % RING], packet + HEADER + i * BLOCK, BLOCK);
            s->label[label % RING] = label + 1;
        }
        while (s->label[s->next % RING] == s->next + 1)
            s->next++;
        if (from == 0 && !synced && seat > 0 && s->next > s->first) {
            /* adopt the host's timeline and delay; start an own stream on it */
            delay = packet[6] ? packet[6] : delay;
            frame_label = own_first = s->next;
            last_target = 0;
            epoch = fnv(epoch + 1, &s->heard, 4) | 1;
            for (i = 0; i < SEATS; i++)
                if (streams[i].known) {
                    struct stream *o = &streams[i];
                    o->acked = 0;
                    o->floor = o->first > frame_label - delay ? o->first : frame_label - delay;
                    o->next = o->floor;         /* acknowledged from here, so the missing ones are repeated */
                    while (o->label[o->next % RING] == o->next + 1)
                        o->next++;
                }
            synced = 1;
            fprintf(stderr, "net: joined the host at frame %u, delay %d\n", (unsigned)frame_label, delay);
        }
    }
}

/* Is the block `label` of that seat part of what this player is given? */
static int due(int who, uint32_t label)
{
    return streams[who].known && label >= streams[who].floor;
}

static void start_session(void)
{
    int i;
    for (i = 0; i < SEATS; i++)
        streams[i].acked = 0;
    epoch = fnv(epoch + 1, &token, 4) | 1;
    connected = available = 0;
    frame_label = own_first = FIRST_LABEL;
    last_target = 0;
    change_at = 0;
    synced = seat == 0;
}

static void leave(void)
{
    uint8_t packet[HEADER];
    if (configured == 1 && synced) {
        header(packet, T_END, 0, 0);
        ss_plat_net_send(packet, HEADER);
        ss_plat_net_send(packet, HEADER);
    }
    synced = 0;
    ss_plat_net_farewell(NULL, 0);
}

/* ---- skip scene, online: every player votes, the host names the frame ---- */

int ss_net_active(void)
{
    return configured == 1 && synced;
}

unsigned long ss_net_label(void)
{
    return frame_label;
}

void ss_net_vote(int wants)
{
    my_vote = wants != 0;
}

void ss_net_votes(int *have, int *need)
{
    int i;
    *have = my_vote;
    *need = 1;
    for (i = 0; i < SEATS; i++)
        if (i != seat && streams[i].known) {
            *have += streams[i].vote;
            *need += 1;
        }
}

/* 1 once, on the frame everybody starts skipping. The host picks a frame far enough ahead for all to learn it. */
int ss_net_skip_now(void)
{
    int have, need;
    if (!ss_net_active())
        return 0;
    ss_net_votes(&have, &need);
    if (seat == 0 && !skip_at && need > 1 && have == need)
        skip_at = frame_label + 2u * (uint32_t)delay + 30;
    if (skip_at && frame_label >= skip_at) {
        skip_done = skip_at;
        skip_at = 0;
        my_vote = 0;
        return 1;
    }
    return 0;
}

/* MultiSioInit */
void __wrap_sub_0800170C(void)
{
    __real_sub_0800170C();
    if (configured < 0)
        configure();
    if (configured != 1)
        return;
    leave();
    start_session();
    send_hello();
}

/* The link task ended its session (called from the game through games/<game>/port/patches.json). */
void ss_net_leave(void)
{
    leave();
}

/* MultiSioMain: one block out, the blocks of frame F - delay in. */
uint32_t __wrap_sub_08001804(void *sendp, void *recvp)
{
    uint32_t now, target, received = 0, waited_since, hash = 2166136261u;
    int i, told = 0, waited = 0;

    if (configured != 1)
        return __real_sub_08001804(sendp, recvp);
    now = ss_plat_ticks_ms();
    if (seat < 0 || now - last_hello > 2000)
        if (now - last_hello > 250)
            send_hello();
    receive();
    if (seat == 0 && !synced)
        start_session();
    while (gather > 1) {        /* tests: nobody moves until the whole table is seated */
        int seen = 0;
        for (i = 0; i < SEATS; i++)
            seen += streams[i].known;
        if (synced && seen >= gather - 1) {
            gather = 0;
            break;
        }
        now = ss_plat_ticks_ms();
        if (seat < 0 && now - last_hello > 250)
            send_hello();
        if (synced && now - last_send >= 30) {
            memcpy(own[frame_label % RING], sendp, BLOCK);
            send_data();
        }
        ss_plat_net_wait(2);
        receive();
        if (seat == 0 && !synced)
            start_session();
    }
    IO16(0x128) = (uint16_t)(0x6003 | (seat > 0 ? seat << 4 | 4 : 0) | (seat >= 0 ? 8 : 0));
    status(-1);
    if (!synced)
        return 0;               /* a console with no parent on the cable hears nothing, itself included */

    if (farewell_epoch != epoch) {          /* closing the window says goodbye too */
        uint8_t packet[HEADER];
        header(packet, T_END, 0, 0);
        ss_plat_net_farewell(packet, HEADER);
        farewell_epoch = epoch;
    }
    memcpy(own[frame_label % RING], sendp, BLOCK);
    send_data();
    /* a longer delay, decided by the host, takes effect at the announced frame on every player */
    if (seat == 0 && auto_delay && !change_at && delay < 12) {      /* 12 frames cover 200 ms each way */
        int asked = raise_request != raise_seen_own;
        raise_seen_own = raise_request;
        for (i = 0; i < SEATS; i++)
            if (i != seat && streams[i].known && streams[i].raise != streams[i].raise_seen) {
                streams[i].raise_seen = streams[i].raise;
                asked = 1;
            }
        if (asked) {
            new_delay = delay + 1;
            change_at = frame_label + 2u * (uint32_t)delay + 30;
        }
    }
    if (change_at && frame_label >= change_at) {
        fprintf(stderr, "net: delay %d -> %d frames\n", delay, new_delay);
        delay = new_delay;
        change_done = change_at;
        change_at = 0;
    }
    target = frame_label - (uint32_t)delay;
    if (last_target && target <= last_target) {
        /* the frame after the delay grew: its blocks were already given, so this one brings nothing */
        frame_label++;
        return (seat == 0 ? 0x80 : 0) | connected << 8 | (available >> 3) << 15;
    }
    last_target = target;
    if (frame_label % 120 == 0) {
        /* Is the delay too short for this connection? Waiting a little is normal: whoever runs ahead waits for
         * the others every frame, whatever the delay. Too short shows as speed: the session falls behind real
         * time, because each frame has to wait for blocks that cannot have arrived yet. So: the last 120 frames
         * took over 4% longer than they should, and at least 40 ms of that was spent waiting. */
        static uint32_t window_started;
        static int history;                 /* the last three windows, newest in bit 0: 1 = too slow */
        uint32_t elapsed = now - window_started;
        int slow = window_started && elapsed > 2090 && elapsed < 6000 && stall_frames >= 40;
        history = ((history << 1) | slow) & 7;
        /* one slow window is a hiccup of the connection (Wi-Fi does that) and passes by itself; a longer delay
         * is for good, so it takes two slow windows out of three to ask for one */
        if (slow && (history & 6))
            raise_request = (raise_request + 1) & 255;
        quiet_windows = slow ? 0 : quiet_windows + 1;
        window_started = now;
        stall_frames = 0;
    }
    waited_since = now;
    for (;;) {
        int missing = -1;
        for (i = 0; i < SEATS; i++)
            if (i != seat && due(i, target) && streams[i].next <= target)
                missing = i;
        if (missing < 0)
            break;
        waited = 1;
        ss_plat_net_wait(2);
        receive();
        if (!synced)
            return 0;
        now = ss_plat_ticks_ms();
        if (now - last_send >= 30)
            send_data();        /* carries the acknowledgements, so the other side repeats what is missing */
        if (!told && now - waited_since > 500) {
            told = 1;
            fprintf(stderr, "net: waiting for player %d\n", missing + 1);
            status(missing);
        }
        for (i = 0; i < SEATS; i++)
            if (streams[i].known && now - streams[i].heard > (uint32_t)timeout_ms)
                drop(i, "timed out");
    }
    if (waited)
        stall_frames += (int)(ss_plat_ticks_ms() - waited_since);       /* milliseconds waited in this window */
    for (i = 0; i < SEATS; i++) {
        const uint8_t *block = NULL;
        if (i == seat)
            block = target >= own_first ? own[target % RING] : sendp;
        else if (due(i, target))
            block = streams[i].block[target % RING];
        if (block) {
            memcpy((uint8_t *)recvp + i * BLOCK, block, BLOCK);
            received |= 1u << i;
            hash = fnv(hash, block, BLOCK);
        }
    }
    if (log_file && target >= own_first)
        fprintf(log_file, "%u %X %08X\n", (unsigned)target, (unsigned)received, (unsigned)hash);
    frame_label++;
    connected |= received;
    if (connected && available < 8)
        available++;
    return received | (seat == 0 ? 0x80 : 0) | connected << 8 | (available >> 3) << 15;
}
