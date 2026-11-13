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
 *   - A connection that is gone for a while is not the end ("a connection that was gone", below): a player that
 *     hears nothing at all knows it is its own connection and gives nobody up; the others keep it as a
 *     standing character and take it in again when its packets come back; the host is waited for longer than
 *     anybody else, because without it the session is over.
 *
 * The delay is as short as the connection allows. Unless SS_NET_DELAY fixes it, a session starts at two frames
 * (the cable's own is one) and the host raises it by one whenever a player reports that it keeps having to wait.
 * It is raised at a frame of the timeline announced in advance; for that one frame nobody is given new blocks,
 * which the game takes as it takes a noisy cable. It is never lowered during a session: that would skip blocks.
 *
 * Environment: SS_NET=host:port (the relay), SS_NET_ROOM=name, SS_NET_DELAY=frames (fixed; automatic if unset or 0),
 * SS_NET_TIMEOUT=milliseconds of silence before a player is dropped (default 15000),
 * SS_NET_HOST_TIMEOUT=the same for the host (default 60000; setting SS_NET_TIMEOUT alone sets both),
 * SS_NET_AWAY=milliseconds this player waits for its own connection to come back (default 120000),
 * SS_NET_LOG=file (one line
 * per delivered frame: label, players, hash of the blocks; identical on every player), SS_NET_PLAYERS=n (tests:
 * wait at the start of the session until n players are in the room), SS_NET_MSG_LOG=file (the message channel,
 * see "messages" below: one line per message handed over and per message dispatched; with SS_NET_MSG_DUMP=1 the
 * message itself follows in hexadecimal), SS_NET_TEST_CHATTER and SS_NET_TEST_MSG (tests: messages of the test's
 * own, see test_chatter and test_message), SS_NET_JOIN=1 (off by default: a program started later is let into
 * the game in progress, see "joining a game in progress" below and docs/JOIN_IN_PROGRESS.md; every player of
 * the session needs it, they meet in a room of their own).
 */
#include "shiningsource/runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO16(o) (*(volatile uint16_t *)(uintptr_t)(SS_IO + (o)))

enum { BLOCK = 24, SEATS = 4, RING = 256, MAX_BLOCKS = 40, HEADER = 60, FIRST_LABEL = 1000 };
enum { T_HELLO = 1, T_WELCOME, T_DATA, T_END };
/* The relay forwards nothing longer than this, so no packet may be. */
enum { PACKET_MAX = 1200 };
/* The message channel (see "messages" below): the longest message, an entry of a stream (the sender's seat and
 * the message), how many entries each side keeps, and the fixed start of its part of a packet. */
enum { MSG_BYTES = 2 + 0x80, MSG_ENTRY = 1 + MSG_BYTES, MSG_OUT = 4096, MSG_IN = 128, MSG_MARK = 0x5A,
       MSG_HEAD = 6 + (SEATS - 1) * 8 };
/* A connection that was gone (see below): how long nothing at all may come from the relay before this player
 * takes its own connection to be gone, how far behind the newest message a player may be and still be served
 * by the packet everybody gets, and how long the host may leave this player out before the session is over
 * for it. */
enum { OFFLINE_AFTER = 2500, MSG_NEAR = 64, DISOWNED_AFTER = 5000 };
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
    /* direct mode (see deliver_direct) */
    uint32_t cursor;            /* next label to hand to the game */
    uint8_t last[BLOCK];        /* the block handed over last, repeated while nothing newer has come */
    int has_last, repeats;
    /* messages (see "messages" below). Of my stream, to that seat: */
    uint32_t msg_start;         /* the first entry that seat is to have: it was not here for older ones */
    uint32_t msg_acked;         /* the next entry that seat expects */
    /* and of that seat's stream, to me: */
    int in_valid;               /* that seat has said where its stream to me starts */
    uint32_t in_next;           /* the next number expected from it */
    uint32_t in_head, in_tail;  /* entries put into the ring below, and entries the game has taken (the host's
                                 * stream only: the host itself keeps what it receives in its own stream) */
    uint8_t in_size[MSG_IN];
    uint8_t in_data[MSG_IN][MSG_ENTRY];
};

static int configured = -1, gather, seat = -1, synced, delay = 4, timeout_ms = 15000;
static uint32_t room, token, epoch, farewell_epoch, frame_label, own_first, connected, available, last_hello, last_send;
static struct stream streams[SEATS];
static void start_session(void);

/* What happens to the session, in words: to the console and, with the moment it happened, to shiningsource_net.log
 * beside the program. A game started from the launcher has no console, and "a companion vanished" can only be
 * understood afterwards from what each player's program saw. A line per event; the file is bounded: past
 * NET_LOG_LIMIT it becomes shiningsource_net.old.log (the one before that is dropped) and a new one starts, so
 * the last 256 to 512 KiB are kept. If the file cannot be renamed nothing is lost: it is appended to as before
 * and the change is tried again later. What a line holds: the system's millisecond counter, the frame, the
 * seat, and the event; the first line of a session names the relay's address and the room. */
enum { NET_LOG_LIMIT = 256 * 1024 };
static void net_say(const char *format, ...)
{
    static const char name[] = "shiningsource_net.log", old[] = "shiningsource_net.old.log";
    static long bytes = -1;     /* the size of the file, counted once from what earlier sessions left */
    char text[200];
    va_list arguments;
    FILE *file;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    fprintf(stderr, "%s", text);
    if (getenv("SS_FRAMES"))
        return;                 /* test runs keep their own logs */
    if (bytes < 0) {
        bytes = 0;
        if ((file = fopen(name, "rb")) != NULL) {
            char passed[1024];
            size_t got;
            while ((got = fread(passed, 1, sizeof passed, file)) > 0)
                bytes += (long)got;
            fclose(file);
        }
    }
    if (bytes > NET_LOG_LIMIT) {
        remove(old);
        bytes = rename(name, old) == 0 ? 0 : bytes - NET_LOG_LIMIT / 4;
    }
    if ((file = fopen(name, "ab")) != NULL) {
        int put = fprintf(file, "%10u ms  frame %lu  seat %d  %s", (unsigned)ss_plat_ticks_ms(), (unsigned long)ss_frame_number(), seat + 1, text);
        bytes += put > 0 ? put : 0;
        fclose(file);
    }
}
static uint8_t own[RING][BLOCK];
static char room_name[32] = "default", status_line[96];
static FILE *log_file;
static int my_vote;
static int auto_delay = 1, stall_frames, raise_request, raise_seen_own, new_delay, quiet_windows;
static uint32_t change_at, change_done, last_target;
static uint32_t skip_at, skip_done;
static int session_open;                /* the game's link task has started a session that was not left */
static uint32_t leaving_since;          /* when the game's link task ended, 0 if it runs */
static const void *last_sendp;         /* the game's send block, while its link task runs */
static uint32_t flushed_label;          /* the label whose block went out at the end of its frame, 0 if none */
enum { TRAILER = 12, TRAILER_MARK = 0xA5 };
enum { DIRECT_HOLD = 30 };
static int direct = 1;          /* the default transport; a fixed SS_NET_DELAY selects the stepped one */
/* my stream of entries, by number. The host's is the session's one order of all messages: kept until every
 * player has an entry and the host's own game has taken it (out_own). Another player's holds its own messages
 * on their way to the host: kept until the host has them. */
static uint32_t out_base = 1, out_next = 1, out_own = 1;
static uint8_t out_size[MSG_OUT], out_data[MSG_OUT][MSG_ENTRY];
static int relay_fresh;         /* the host put a player's message into its stream: pass it on without waiting */
static FILE *msg_log;
static void msg_reset(void);
/* players that left a game in progress (see drop): the seats, and the block the game keeps being given for each */
static uint32_t gone;
static uint8_t gone_block[SEATS][BLOCK];
static int in_play;             /* a message has passed in this session: the game is past its lobby */
static uint32_t old_epoch[SEATS];       /* the stream each seat had before its current one */
static uint32_t host_lost;      /* the host left a game in progress: the connected bits the game had then, else 0 */
/* a connection that was gone (see below) */
static int host_timeout_ms = 60000, away_ms = 120000;
static uint32_t relay_heard;    /* when the last packet of any kind came from the relay */
static uint32_t offline_since;  /* since when nothing comes from it: this player's own connection is gone; 0 if not */
static int host_knew;           /* the host has acknowledged this player's current stream */
static uint32_t disowned_since; /* since when the host's packets leave this player's stream out, 0 if they do not */
static struct {                 /* what is kept of a player given up for silence, so that it can come back */
    uint32_t epoch;             /* the stream it had; 0: it cannot come back */
    uint32_t since;             /* when it was given up */
    uint32_t msg_start, msg_acked, in_next;
    int in_valid;
} away[SEATS];

/* joining a game in progress (see below) */
enum { J_INFO = 0xF1, J_ADMIT = 0xF2, J_SNAP = 0xF4, J_SNAP_END = 0xF5 };      /* this layer's own entries of the order */
enum { JR_NONE, JR_LOBBY, JR_PARKED, JR_SNAP, JR_BRINGUP, JR_LIVE };
enum { JOIN_PARKED = 0x40 };            /* in the header's delay byte: this candidate's game waits at its barrier */
enum { SNAP_KINDS = 6, SNAP_GLOBALS = 4 };
static int join_mode;                   /* SS_NET_JOIN=1 */
static uint32_t candidate;              /* seats whose stream is here but whose player is not in the game yet */
static uint32_t cand_owed, cand_told, cand_ready, cand_admitted;       /* the host's: info to send, sent, parked, let in */
static uint32_t cand_info_at[SEATS];    /* the host's: the number of the entry that told the candidate about the session */
static int join_role;                   /* this program is entering a game in progress: how far it is (JR_*) */
static int join_anchor;                 /* the game is at the point of a map's set-up where a player can be let in */
static int join_first_init;             /* the newcomer's first set-up of a map: the part the others did before is left out */
static uint8_t lobby_byte;              /* the host's lobby settings (byte 10 of its block when it said "GO") */
static uint8_t join_info[40], join_admit[24];
static uint32_t grow_pending;           /* seats the game must make a player for (ss_net_join_grow) */
static uint32_t join_prefill;           /* the newcomer's: seats whose barrier token it is given again */
static int tokens_out;                  /* own barrier tokens handed over that have not come round in the order */
static uint32_t joined;                 /* seats that were let into the game in progress */
static int join_drop_tokens;            /* the newcomer's: barrier tokens of its session's start still to be dropped */
static int join_admit_seen;             /* the newcomer's: the entry that lets it in has arrived */
static int lobby_done;                  /* the host has said "GO" in the game's lobby: the session's seats are fixed */
static uint8_t snap_size[SEATS][SNAP_KINDS], snap_data[SEATS][SNAP_KINDS][MSG_BYTES];
static uint8_t snap_global_size[SNAP_GLOBALS], snap_global_who[SNAP_GLOBALS], snap_global[SNAP_GLOBALS][MSG_BYTES];

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
    direct = auto_delay;
    if (auto_delay) delay = 2;
    if (delay > 30) delay = 30;
    if (getenv("SS_NET_TIMEOUT"))
        host_timeout_ms = timeout_ms = (int)strtoul(getenv("SS_NET_TIMEOUT"), NULL, 10);
    if (getenv("SS_NET_HOST_TIMEOUT"))
        host_timeout_ms = (int)strtoul(getenv("SS_NET_HOST_TIMEOUT"), NULL, 10);
    if (getenv("SS_NET_AWAY"))
        away_ms = (int)strtoul(getenv("SS_NET_AWAY"), NULL, 10);
    if (getenv("SS_NET_PLAYERS"))
        gather = (int)strtoul(getenv("SS_NET_PLAYERS"), NULL, 10);
    if (getenv("SS_NET_LOG"))
        log_file = fopen(getenv("SS_NET_LOG"), "w");
    if (getenv("SS_NET_MSG_LOG"))
        msg_log = fopen(getenv("SS_NET_MSG_LOG"), "w");
    room = fnv(2166136261u, room_name, (int)strlen(room_name));
    {
        /* Players only meet if they run the same game and the same rules: the room is the name together with
         * the cartridge's contents and the version of this protocol. Another version of the port or another
         * ROM lands in another room instead of producing a session that drifts apart. */
        static const uint32_t rules = 5;        /* raise when the transport or anything the players must agree on changes */
        room = fnv(room, (const void *)(uintptr_t)SS_ROM, 0x1000000);
        room = fnv(room, &rules, sizeof rules);
        room = fnv(room, &direct, sizeof direct);      /* the two transports do not mix */
        /* SS_NET_JOIN=1: a player can be let into a game in progress ("joining a game in progress", below). The
         * players of such a session all have to know how, so they meet in a room of their own; without the
         * setting the room is the one it always was and nothing below is ever reached. Only with the direct
         * transport (on the stepped one a newcomer has no place on the timeline of the game in progress). */
        join_mode = direct && getenv("SS_NET_JOIN") && getenv("SS_NET_JOIN")[0] == '1';
        if (join_mode)
            room = fnv(room, "join", 4);
    }
    token = getenv("SS_NET_TOKEN") ? (uint32_t)strtoul(getenv("SS_NET_TOKEN"), NULL, 10)
                                   : fnv(ss_plat_ticks_ms() * 2654435761u, &text, sizeof text) | 1;
    if (ss_plat_net_open(host, port)) {
        net_say("net: cannot reach %s:%d, playing offline\n", host, port);
        return 0;
    }
    net_say("net: relay %s:%d, room '%s', delay %d\n", host, port, room_name, delay);
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
    packet[6] = (uint8_t)(delay | (join_role == JR_PARKED ? JOIN_PARKED : 0));
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

/* The message channel's part of a packet, written at `part` (see send_data and "messages" below): for every
 * other seat where my stream to it starts and which entry of its stream I expect next, then the entries of my
 * stream from `first` up to `stop`, as many as fit into `room` bytes. Returns the size of the part; `next` is
 * given the entry after the last one that fitted. */
static int message_part(uint8_t *part, int room, uint32_t first, uint32_t stop, uint32_t *next)
{
    uint8_t *entry = part + 6;
    uint32_t number;
    int i, size = MSG_HEAD, carried = 0;
    for (i = 0; i < SEATS; i++) {
        struct stream *s = &streams[i];
        if (i == seat)
            continue;
        /* A candidate for a game in progress that has been told about the session is given nothing more of the
         * order until it is let in: the start of its stream moves along with the newest entry, here, where
         * the packet is made (an entry appended in this frame must not slip through). */
        if (seat == 0 && ((candidate & cand_told & ~cand_admitted) >> i & 1) && s->msg_acked > cand_info_at[i])
            s->msg_start = s->msg_acked = out_next;
        /* (and no start at all before the host has put the entry that tells it about the session into the
         * order: its stream starts there) */
        wr32(entry, s->known && !(seat == 0 && (candidate & ~cand_told) >> i & 1) ? s->msg_start : 0);
        wr32(entry + 4, s->known && s->in_valid ? s->in_next : 0);
        entry += 8;
    }
    for (number = first; number != stop && carried < 255; number++, carried++) {
        int length = out_size[number % MSG_OUT];
        if (size + 1 + length > room)
            break;
        part[size] = (uint8_t)length;
        memcpy(part + size + 1, out_data[number % MSG_OUT], (size_t)length);
        size += 1 + length;
    }
    part[0] = MSG_MARK;
    part[1] = (uint8_t)carried;
    wr32(part + 2, first);
    *next = number;
    return size;
}

/* Everything the others have not acknowledged, up to the block of the current frame. */
static void send_data(void)
{
    uint8_t packet[PACKET_MAX];
    uint32_t from = frame_label, label;
    int i, count = 0, size;

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
    size = HEADER + count * BLOCK + TRAILER;
    {   /* after the trailer: the message channel. For every other seat, where my stream to it starts and which
         * entry of its stream I expect next (that is the acknowledgement); then every entry of my stream that
         * has not been acknowledged, oldest first, as many as fit. They are repeated in every packet until
         * acknowledged, like the blocks, so a lost packet costs nothing. The host's stream is for everybody: a
         * player that has been silent for half a second is not sent to (what it lacks may be more than a
         * packet holds, and the players that are listening must not wait behind it; it gets everything when
         * it speaks again). Another player's stream is for the host alone.
         *
         * When it speaks again it may lack hundreds of entries (its connection was gone for a while). The
         * relay passes every packet to everybody, and a packet holds one run of entries, so serving it from
         * this packet would make the others wait for their next message until it has caught up. So this
         * packet starts at what the players that are keeping up lack (within MSG_NEAR entries of the newest),
         * and a player further behind gets packets of its own in the same frame, with the same blocks and
         * the entries it lacks; to the others those repeat what they have, which costs them nothing. */
        uint32_t first = out_next, kept_up = out_next, next, now = ss_plat_ticks_ms();
        int extra;
        for (i = 0; i < SEATS; i++) {
            struct stream *s = &streams[i];
            if (i == seat || !s->known || !(seat == 0 ? now - s->heard < 500 : i == 0))
                continue;
            if (seat == 0 && ((candidate & ~cand_told) >> i & 1))
                continue;       /* a candidate that has not been told about the session: its stream has not begun */
            if (s->msg_acked < first)
                first = s->msg_acked;
            if (s->msg_acked < kept_up && out_next - s->msg_acked <= MSG_NEAR)
                kept_up = s->msg_acked;
        }
        if (first < out_base)
            first = out_base;
        if (kept_up < first)
            kept_up = first;
        ss_plat_net_send(packet, size + message_part(packet + size, PACKET_MAX - size, kept_up, out_next, &next));
        for (extra = 0; extra < 2 && first < kept_up; extra++, first = next) {
            ss_plat_net_send(packet, size + message_part(packet + size, PACKET_MAX - size, first, kept_up, &next));
            if (next == first)
                break;
        }
    }
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
    else if (offline_since)
        snprintf(line, sizeof line, "online: no connection, waiting for it to come back");
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

/* A player left, crashed or fell silent.
 *
 * In the lobby the game is simply told: the seat's bits go and the lobby shows one player fewer. Once the game
 * is being played that would end it for everybody: the game counts the connected bits and takes the players to
 * be seats 0..n-1, so a missing bit in the middle hides the last player, and a seat that is counted but no
 * longer heard runs the link task into its error screen after 80 frames. So a player that leaves a game in
 * progress stays as it was last seen: its seat keeps its connected bit and the game is given its last block
 * every frame, with the message bytes saying "idle". The others see its character stand where it was and play
 * on. Its messages that reached the host are in the host's order and are acted on by everybody; the channel
 * stops waiting for its acknowledgements (its stream is forgotten); the game's "is everybody ready" check
 * passes over it (ss_net_msg_gone, used from games/<game>/port/patches.json). Another stream that later shows
 * up on that seat is ignored until this player's own session ends: the game has no way to let somebody in
 * halfway. The same stream may come back: see "a connection that was gone" below.
 *
 * Seat 0 is the exception. It is the game's parent: only it decides enemies and items, and the seat numbers
 * cannot change during a game, so nobody can take that over. When the host leaves a game in progress the others
 * are told what a cable tells when the parent console is pulled out: everybody still counted, nobody heard, and
 * the driver's error bit. The game's link task then runs into its own link error handling.
 *
 * `may_return`: it fell silent without a goodbye, so it may be a connection that comes back. */
static void drop(int who, const char *why, int may_return)
{
    struct stream *s = &streams[who];
    /* the block it was last seen with: the one handed over last (direct), or the newest received (stepped) */
    const uint8_t *seen = s->has_last ? s->last : s->next > s->floor ? s->block[(s->next - 1) % RING] : NULL;
    int stays = in_play && who != 0 && who != seat && s->known && seen && (connected & 1u << who);

    if (candidate >> who & 1) {
        /* a program that wanted to join the game in progress and was not let in yet: the game never knew it.
         * (On a seat whose player left, that player's standing character stays as it was.) */
        net_say("net: player %d, which wanted to join, %s\n", who + 1, why);
        candidate &= ~(1u << who);
        cand_owed &= ~(1u << who);
        cand_told &= ~(1u << who);
        cand_ready &= ~(1u << who);
        cand_admitted &= ~(1u << who);
        memset(s, 0, sizeof *s);
        return;
    }
    if (s->known)
        net_say("net: player %d %s%s\n", who + 1, why, stays ? " (its character stays)" : "");
    if (stays) {
        memcpy(gone_block[who], seen, BLOCK);
        gone_block[who][8] = (uint8_t)who;      /* the message bytes as a sender leaves them between messages */
        gone_block[who][9] = 0x80;
        memset(gone_block[who] + 10, 0, 10);
        memset(gone_block[who] + 20, 0x80, 4);  /* and as a receiver leaves them while it waits */
        gone |= 1u << who;
        /* what taking it in again needs (see revive): which stream it was and where the message channel
         * stood with it. Only the direct transport can take a player in again: on the stepped one it stood
         * still on the timeline while the others went on. */
        away[who].epoch = may_return && direct ? s->epoch : 0;
        away[who].since = ss_plat_ticks_ms();
        away[who].msg_start = s->msg_start;
        away[who].msg_acked = s->msg_acked;
        away[who].in_valid = s->in_valid;
        away[who].in_next = s->in_next;
        /* forget the stream */
        s->known = 0;
        s->epoch = s->first = s->floor = s->next = s->cursor = s->heard = s->acked = 0;
        memset(s->label, 0, sizeof s->label);
        s->vote = s->raise = s->raise_seen = 0;
        s->has_last = s->repeats = 0;
        s->in_valid = 0;
        s->msg_start = s->msg_acked = 0;
        return;
    }
    if (who == 0 && seat > 0 && in_play && s->known) {
        host_lost = connected;
        /* The game is being told that the session is over and will start again. If the host's packets come
         * after all (its connection was gone for longer than it is waited for), they are not a new host to
         * join: taken for one, this player would sit in the middle of a game its own game has given up. */
        old_epoch[0] = s->epoch;
    }
    memset(s, 0, sizeof *s);
    connected &= ~(1u << who);
    if (who == 0 && seat > 0)
        synced = 0;             /* the timeline is the host's */
}

/* ---- a connection that was gone ----
 * A player's program keeps running while its connection is away (Wi-Fi lost for some seconds, a router that
 * restarts). Nothing of the session is lost by that in itself: the game replicates state, so the blocks that
 * were not delivered are simply out of date, and the messages are kept and repeated until acknowledged. What
 * ended such sessions was each side deciding for good that the other had left. So:
 *
 *   - The player that is cut off notices that nothing at all comes from the relay, not even the answers to
 *     its own HELLOs (which it then sends twice a second). That is its own connection, not the others
 *     leaving, so it gives nobody up; when packets come again every player's clock starts anew. This lasts at
 *     most SS_NET_AWAY (two minutes): the relay may also be gone for good.
 *   - A stream that was silent for more than MAX_BLOCKS frames has a hole the sender will not fill (a packet
 *     only reaches that far back). The receiver steps over the hole (receive): state that old is of no use.
 *   - The others give a silent player up after SS_NET_TIMEOUT as before and keep its character standing
 *     (drop). They remember its stream. When packets of that same stream come again the seat is taken in
 *     again (revive): its blocks are handed over as before, and the message channel goes on where it stood.
 *     For that the host keeps every entry of its stream that such a player has not acknowledged, as long as
 *     there is room for them (msg_room), and remembers which of the player's entries it expects next. So
 *     the player is given every message it missed, in the order, each once, as after a very long lag, and
 *     its own messages of that time take their place in the order when they arrive. Nothing is skipped, so
 *     its game stays the game the others have. While it catches up the others are not kept waiting
 *     (send_data).
 *   - If the host no longer has those entries (MSG_OUT of them passed while the player was away) it cannot be
 *     brought up to date: to act on later messages without the earlier ones would leave it with enemies and
 *     items the others do not have. The host then keeps it out. The player sees that the host's packets no
 *     longer name its stream and, after DISOWNED_AFTER, is told what it is told when the host leaves: the
 *     session is over for it. It says goodbye, so that it stands for every player and not only for the host.
 *     The same happens with a host that runs an older version and never takes anybody in again.
 *   - The same again when the game passed one of its barriers while the player was given up (ss_net_msg_passed,
 *     called from the game's "does everybody hold this token", games/<game>/port/patches.json). What the
 *     game does at a barrier is not all in the messages: on the world map the host starts the dungeon by a
 *     state in its block, which is seen for as long as the barrier stands and never again. A player that
 *     came back after it was given every message and stayed on the world map for the rest of the session,
 *     with the others' tokens piling up in its rings, while the others played the dungeon
 *     (tools/net_outage_map.sh, "chosen"). And had it followed, its own tokens for the barriers the others
 *     had long passed would have stayed in their rings and made every later barrier count it as ready
 *     before it was. A player that comes back while the barrier still stands (the others wait there for its
 *     token until it is given up) is taken in as before and goes on with them.
 *   - A player given up for silence is expected back for as long as it waits itself (SS_NET_AWAY after it
 *     was given up); then it counts as having left for good (ss_net_msg_left). The world map needs every
 *     player's choice: there one that may come back is waited for, one that left for good is passed over.
 *   - The host is waited for longer (SS_NET_HOST_TIMEOUT, a minute): giving a player up costs the others
 *     nothing, giving the host up ends the session for everybody. While the host is away nobody's messages
 *     get their place in the order, so the players walk about and nothing else happens until it is back or
 *     given up.
 * Nothing in the packets changed for this; a player without it meets one with it as before. */

/* How long that seat may be silent before it is given up. */
static uint32_t patience(int who)
{
    return (uint32_t)(who == 0 && direct ? host_timeout_ms : timeout_ms);
}

/* Is this player's own connection gone? Called every frame and while waiting, after receive(). */
static void connection_check(uint32_t now)
{
    /* (a packet may have come since `now` was read: that is no silence, not a very long one) */
    uint32_t silent = (int32_t)(now - relay_heard) > 0 ? now - relay_heard : 0;
    int i, is = relay_heard && silent > OFFLINE_AFTER && silent < (uint32_t)away_ms;

    if (is == (offline_since != 0))
        return;
    if (is) {
        offline_since = (now - silent) | 1;
        net_say("net: nothing comes from the server any more; waiting for this player's connection, nobody is given up\n");
    } else if (silent >= (uint32_t)away_ms) {
        offline_since = 0;
        net_say("net: no connection for %u seconds; not waiting for it any longer\n", (unsigned)(silent / 1000));
    } else {
        net_say("net: the connection is back after %u ms\n", (unsigned)(now - offline_since));
        offline_since = 0;
        for (i = 0; i < SEATS; i++)
            if (streams[i].known)
                streams[i].heard = now;         /* they were not silent: this player was not listening */
        disowned_since = 0;
    }
}

/* Packets of the stream a departed player had come again: its connection was gone and is back. The seat is
 * taken in where it stood. `first` is the first label of its stream, `label` the first one this packet brings:
 * the blocks before it are a hole (see above). Until the first new block is handed over the game keeps being
 * given the block the standing character had. */
static void revive(int who, uint32_t first, uint32_t label)
{
    struct stream *s = &streams[who];

    memset(s, 0, sizeof *s);
    s->known = 1;
    s->epoch = away[who].epoch;
    s->first = first;
    s->floor = s->next = label;
    memcpy(s->last, gone_block[who], BLOCK);
    s->has_last = 1;
    s->heard = ss_plat_ticks_ms();
    if (seat == 0) {
        s->msg_start = away[who].msg_start;
        s->msg_acked = away[who].msg_acked;
        s->in_valid = away[who].in_valid;
        s->in_next = away[who].in_next;
    } else {
        s->msg_start = s->msg_acked = out_next;         /* two players that are not the host exchange no messages */
    }
    gone &= ~(1u << who);
    away[who].epoch = 0;
    net_say("net: player %d is back: its connection was gone (%u messages to catch up)\n", who + 1,
            seat == 0 ? (unsigned)(out_next - s->msg_acked) : 0u);
}

/* ---- messages ----
 * What the game sends once (an enemy appears, an item is dropped or taken, a hit, a player's name and stats,
 * a change of map) it passes over the cable as a message of up to sixteen 8-byte pieces, one step a frame in
 * bytes 8..23 of the block: announce, wait for every player's answer, a piece, wait again, and so on (games/ss2:
 * sub_08066720 and sub_0806690C). Over a cable a step is a frame; over the network each one is a trip there and
 * back, so a four-piece message took about five round trips and the next one of that player waited behind it.
 *
 * Here a whole message travels at once. The game's sender hands it over (ss_net_msg_send) and the game's
 * receivers take it (ss_net_msg_next, ss_net_msg_recv); the game's own code for both is patched to do so when
 * a session is online (games/<game>/port/patches.json).
 *
 * One order for everybody. On the cable a message was complete on every console within a frame or two of the
 * others, because each step waited for everybody's answer; so all consoles acted on all messages in practically
 * the same order, and the game leans on that: what one player's message does can depend on what another
 * player's message did before it (an enemy struck by one and killed by another; the request to pick an item up
 * and the host's record of it; a new enemy with the identifier of one that just died). Messages that travel on
 * their own from each sender reach different machines in different orders. So there is one order, the host's,
 * and every machine acts in it, the sender on its own messages too:
 *   - a player's message goes to the host only (its stream of entries, repeated until the host has it);
 *   - the host puts every message, its own and the others' as they reach it, into one stream: seat of the
 *     sender, then the message. That stream is the order. It goes to every player, repeated until each has
 *     acknowledged it, and is given to the game in that order, each entry exactly once;
 *   - nobody acts on a message before it has come round in the host's stream, so a player's own message
 *     costs it a trip to the host and back, and reaches the others in two trips instead of one. The relay is
 *     near the players; that is a few frames.
 * Entries are numbered per stream from 1, anew for every stream (epoch). A player that joins a stream late is
 * told the number it starts at: what was sent before it was there was never meant for it.
 *
 * A player that left (drop) is no longer waited for. What the host had of its messages is in the order and
 * reaches everybody; what the host never received nobody acts on. When the host leaves the session ends.
 *
 * Nothing in the game asks whether its queue of messages is empty; where it needs everybody to be ready it
 * sends a token and waits until it holds everybody's (sub_08067C18 and sub_08067C08), so the sender may hand a
 * message over and go on at once. */

static void msg_note(char what, int who, const uint8_t *data, int size)
{
    if (msg_log) {
        /* the last field is a hash of the whole message: two logs then show not only the same kinds of
         * messages in the same order but the same messages, none twice and none missing */
        static int whole = -1;  /* SS_NET_MSG_DUMP=1: an eighth field, the message itself in hexadecimal */
        int i;
        if (whole < 0)
            whole = getenv("SS_NET_MSG_DUMP") != NULL;
        fprintf(msg_log, "%u %u %c %d %02X %d %08X", (unsigned)ss_plat_ticks_ms(), (unsigned)frame_label, what, who,
                size > 0 ? data[0] : 0, size, (unsigned)fnv(2166136261u, data, size));
        for (i = 0; whole && i < size; i++)
            fprintf(msg_log, i ? "%02X" : " %02X", data[i]);
        fprintf(msg_log, "\n");
        fflush(msg_log);
    }
}

/* SS_NET_TEST_CHATTER=N (tests): once the game is being played, every N frames this player hands over a
 * message of its own with a running number in it. The scripted sessions reach a field where only the host has
 * anything to say; with this every player's stream carries traffic, so a test of a lost connection also covers
 * the way to the host. The tag (0xFE) is none of the game's: its receiver acts on tags 0x81 to 0xB0 and does
 * nothing for any other. */
static void test_chatter(void)
{
    static int every = -1;
    static uint32_t number, last;
    uint8_t message[10];
    if (every < 0)
        every = getenv("SS_NET_TEST_CHATTER") ? (int)strtoul(getenv("SS_NET_TEST_CHATTER"), NULL, 10) : 0;
    if (every <= 0 || !in_play || frame_label - last < (uint32_t)every || (join_role != JR_NONE && join_role != JR_LIVE))
        return;
    message[0] = 0xFE;
    message[1] = 1;             /* one piece of eight bytes, as the game's own messages are built */
    wr32(message + 2, number);
    wr32(message + 6, (uint32_t)seat);
    if (ss_net_msg_send(message, sizeof message)) {
        number++;
        last = frame_label;
    }
}

/* SS_NET_TEST_MSG=F:HEX[,F:HEX ...] (tests): at frame F of this program (or the first one after it at which a
 * session is being played) this player hands over the message whose bytes are HEX: the tag, the number of
 * 8-byte pieces, the pieces. A scripted session can then hold what the buttons alone do not reach. For
 * games/ss2, "9F010100000000000000" is the game's own "everybody changes map" (tag 0x9F, one piece): every
 * player's game leaves the map for the one its character is bound for, from the first field to the castle
 * gate. tools/net_outage_map.sh starts its change of map while a player is away with that. */
static void test_message(void)
{
    static const char *cursor;
    static int asked;
    uint8_t message[MSG_BYTES];
    unsigned long at;
    char *end;
    int size = 0;

    if (!asked) {
        asked = 1;
        cursor = getenv("SS_NET_TEST_MSG");
    }
    if (!cursor || !*cursor || !in_play)
        return;
    at = strtoul(cursor, &end, 10);
    if (*end != ':') {
        cursor = NULL;
        return;
    }
    if (ss_frame_number() < at)
        return;
    for (end++; size < MSG_BYTES; end += 2, size++) {
        int high = end[0] <= '9' ? end[0] - '0' : (end[0] | 32) - 'a' + 10, low;
        if (high < 0 || high > 15)
            break;
        low = end[1] <= '9' ? end[1] - '0' : (end[1] | 32) - 'a' + 10;
        if (low < 0 || low > 15)
            break;
        message[size] = (uint8_t)(high << 4 | low);
    }
    if (size >= 2 && !ss_net_msg_send(message, size))
        return;                 /* no room: again next frame */
    cursor = *end == ',' ? end + 1 : NULL;
}

/* A new stream of mine: its entries are numbered from 1 and nobody has any, and what the others sent to the
 * old one is forgotten (they start again where they tell me to). */
static void msg_reset(void)
{
    int i;
    out_base = out_next = out_own = 1;
    gone = 0;
    in_play = 0;
    host_lost = 0;
    relay_fresh = 0;
    memset(away, 0, sizeof away);
    host_knew = 0;
    disowned_since = 0;
    candidate = cand_owed = cand_told = cand_ready = cand_admitted = 0;
    join_role = JR_NONE;
    join_first_init = 0;
    grow_pending = join_prefill = joined = 0;
    lobby_done = join_admit_seen = join_drop_tokens = 0;
    tokens_out = 0;
    memset(snap_size, 0, sizeof snap_size);
    memset(snap_global_size, 0, sizeof snap_global_size);
    for (i = 0; i < SEATS; i++) {
        streams[i].msg_start = streams[i].msg_acked = 1;
        streams[i].in_valid = 0;
        streams[i].in_head = streams[i].in_tail = 0;
    }
}

/* Room for one more entry in my stream? Entries nobody needs any more are forgotten first: the host's when
 * every player has them and its own game has taken them, another player's when the host has them. */
static int msg_room(void)
{
    uint32_t have = out_next;
    int i;
    if (seat == 0) {
        for (i = 1; i < SEATS; i++)
            if (streams[i].known && streams[i].msg_acked < have && !((candidate & ~cand_told) >> i & 1))
                have = streams[i].msg_acked;    /* (nothing is kept for a candidate that has not been told about the session) */
        if (out_own < have)
            have = out_own;
        /* A player given up for silence may come back (revive) and then needs every entry it has not had.
         * They are kept for it while there is room; when the room is needed it cannot come back any more. */
        for (i = 1; i < SEATS; i++)
            if ((gone >> i & 1) && away[i].epoch && away[i].msg_acked < have) {
                static int keep = -1;           /* SS_NET_KEEP=n (tests): as if there were room for n entries only */
                if (keep < 0)
                    keep = getenv("SS_NET_KEEP") ? (int)strtoul(getenv("SS_NET_KEEP"), NULL, 10) : MSG_OUT - 1;
                if (out_next - away[i].msg_acked < (uint32_t)(keep < MSG_OUT - 1 ? keep : MSG_OUT - 1)) {
                    have = away[i].msg_acked;
                } else {
                    away[i].epoch = 0;
                    net_say("net: player %d has been away for too many messages to be brought up to date; it cannot come back\n", i + 1);
                }
            }
    } else if (streams[0].known) {
        have = streams[0].msg_acked;
    } else {
        have = out_base;        /* no host to take them */
    }
    if (have > out_base)
        out_base = have;
    return out_next - out_base < MSG_OUT;
}

/* ---- joining a game in progress (SS_NET_JOIN=1, off by default; docs/JOIN_IN_PROGRESS.md) ----
 * A program that is started while the others are already playing. What it lacks, and how it is given it:
 *
 *   - The lobby. The newcomer's game opens its link and waits in the game's lobby for the host's "GO". The others
 *     left the lobby long ago, so this layer plays it: the host's block says "GO" with the lobby byte the real
 *     host had, every other seat says "OK", and the game counts the seats the session has plus its own.
 *   - A seat. The game takes the players to be seats 0..n-1 and makes a task and a record for each when the
 *     session starts. The relay gives the newcomer the lowest free seat. Until it is let in it is a candidate:
 *     the others' games are told nothing of it. When it is let in the others' games are given one seat more and
 *     make the record and the task for it then (ss_net_join_grow, games/<game>/port/patches.json); a seat whose
 *     player left (drop) has them already and is simply played by the newcomer from then on.
 *   - A moment. The newcomer's game goes through the start of a session as far as its first barrier (the game's
 *     "is everybody ready" token, tag 0x84) and waits there. The host lets it in when its own game enters a map:
 *     at the message with which the host announces the map's layouts (0x85) inside the map's set-up (the game
 *     tells this layer that it is there: ss_net_join_anchor). Enemies and items of the new map do not exist
 *     yet at that point, so nothing of them has to be copied.
 *   - What it missed. The host keeps the last message of every kind that says what a player or the session is
 *     (name, class, weapon, flags, the two blocks of statistics per player; the session's settings) and puts
 *     them into the order for the newcomer, wrapped so that the others pass over them, then where the session
 *     is (map, area and the fields of the scene the set-up of a map reads) and which players' tokens for the
 *     barrier under way are already in the order (the newcomer is given those again: its rings of tokens then
 *     are the host's at that point of the order, and from there on it is given what the host is given).
 *   - Its place in the game. Released from its barrier, the newcomer's game selects that map (the game's own
 *     function for it, ss_net_join_place) and sets it up as the others do from the point where the host stood,
 *     taking part in the barriers that follow like everybody else.
 *
 * All of this travels in the message channel, as entries with tags the game does not use (the game's are 0x81
 * to 0xB0), in the host's one order: every machine reaches them at the same point of the order. Nothing in the
 * packets' layout changed except one bit in the header's delay byte (JOIN_PARKED). Programs with the setting
 * meet in a room of their own (see configure), so a program without it never sees any of it. */
static const uint8_t *msg_head(int *size);
static const uint8_t snap_kind[SNAP_KINDS] = { 0x81, 0x82, 0x83, 0x8C, 0x8E, 0x8F };
static const uint8_t snap_global_kind[SNAP_GLOBALS] = { 0xAB, 0xAC, 0xAD, 0xAE };

/* The host's: remember the last message of every kind that says what a player or the session is. */
static void join_remember(int who, const uint8_t *data, int size)
{
    int i;
    if (who < 0 || who >= SEATS || size < 1 || size > MSG_BYTES)
        return;
    for (i = 0; i < SNAP_KINDS; i++)
        if (data[0] == snap_kind[i]) {
            memcpy(snap_data[who][i], data, (size_t)size);
            snap_size[who][i] = (uint8_t)size;
        }
    for (i = 0; i < SNAP_GLOBALS; i++)
        if (data[0] == snap_global_kind[i]) {
            memcpy(snap_global[i], data, (size_t)size);
            snap_global_size[i] = (uint8_t)size;
            snap_global_who[i] = (uint8_t)who;
        }
}

/* Put a message of seat `who` at the end of my stream. */
static void msg_append(int who, const uint8_t *data, int size)
{
    uint8_t *entry = out_data[out_next % MSG_OUT];
    entry[0] = (uint8_t)who;
    memcpy(entry + 1, data, (size_t)size);
    out_size[out_next % MSG_OUT] = (uint8_t)(1 + size);
    out_next++;
    if (join_mode && seat == 0 && data[0] < J_INFO)
        join_remember(who, data, size);
}

/* The game's memory, as games/ss2 lays it out (like the test hooks in core/frame.c): the scene record
 * gUnk_03005CF4 points to (map +0, area +0x40, the exit's way +0x44, the callback column +0x36), and the
 * players' rings of barrier tokens (cursors gUnk_03006210, ten words each at gUnk_03006260; the default token
 * is 0x37564, sub_08067C18). */
static uint32_t peek32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, 4);
    return value;
}

static int ring_holds_token(int who)
{
    uint32_t cursor = peek32(0x03006210u + (uint32_t)who * 4), j;
    for (j = 0; j <= cursor && j < 10; j++)
        if (peek32(0x03006260u + (uint32_t)who * 40 + j * 4) == 0x37564u)
            return 1;
    return 0;
}

static int contiguous(uint32_t mask)
{
    return mask && !(mask & (mask + 1));
}

/* The host's: tell a candidate about the session (the lobby byte, which seats the game counts, which of them
 * are players that left and where those stand), in an entry of the order its stream then starts at. */
static void join_tell(void)
{
    int k, i;
    uint8_t info[40];
    for (k = 1; k < SEATS; k++)
        if ((cand_owed >> k & 1) && (candidate >> k & 1))
            break;
    /* one at a time: what a second one is told changes when the first is let in */
    if (k == SEATS || (cand_told & ~cand_admitted) || (candidate & cand_admitted) || grow_pending || !msg_room())
        return;
    cand_owed &= ~(1u << k);
    if (!contiguous(connected | joined | 1u << k)) {
        net_say("net: player %d cannot join: its seat does not follow the seats the game has\n", k + 1);
        return;
    }
    memset(info, 0, sizeof info);
    info[0] = J_INFO;
    info[1] = 4;
    info[2] = (uint8_t)k;
    info[3] = lobby_byte;
    info[4] = (uint8_t)(connected | joined);
    info[5] = (uint8_t)(gone & ~(1u << k));
    for (i = 0; i < SEATS; i++)
        if ((gone >> i & 1) && i != k)
            memcpy(info + 8 + i * 8, gone_block[i], 8);
    cand_info_at[k] = out_next;
    streams[k].msg_start = streams[k].msg_acked = out_next;
    msg_append(0, info, sizeof info);
    cand_told |= 1u << k;
    cand_admitted &= ~(1u << k);
    net_say("net: player %d wants to join the game in progress; it will be let in at the next change of map\n", k + 1);
}

/* The host's game hands over the message that announces a map's layouts, inside the map's set-up: a candidate
 * whose game waits at its barrier is let in here. Before that message go, in the order: the entry that lets
 * it in (for everybody), what it missed (for it alone), and the end of that. */
static void join_admit_now(void)
{
    uint32_t scene = peek32(0x03005CF4u), tokens = 0;
    uint8_t entry[MSG_BYTES];
    int k, i, kind;

    for (k = 1; k < SEATS; k++)
        if ((candidate & cand_told & cand_ready & ~cand_admitted) >> k & 1)
            break;
    if (k == SEATS || scene < SS_EWRAM || scene >= SS_EWRAM + 0x40000 - 0x48)
        return;
    msg_room();
    if (MSG_OUT - (out_next - out_base) < 8 + SEATS * SNAP_KINDS + SNAP_GLOBALS)
        return;                 /* no room for all of it now: at the next map */
    for (i = 0; i < SEATS; i++)
        if ((connected >> i & 1) && ring_holds_token(i))
            tokens |= 1u << i;
    memset(entry, 0, sizeof entry);
    entry[0] = J_ADMIT;
    entry[1] = 2;
    entry[2] = (uint8_t)k;
    entry[3] = (uint8_t)tokens;
    memcpy(entry + 4, (const void *)(uintptr_t)scene, 2);               /* the map */
    memcpy(entry + 6, (const void *)(uintptr_t)(scene + 0x36), 2);
    memcpy(entry + 8, (const void *)(uintptr_t)(scene + 0x40), 8);      /* the area, and the way the exit gave */
    entry[16] = (uint8_t)(gone & ~(1u << k));           /* players that left since it was told about the session */
    streams[k].msg_start = streams[k].msg_acked = out_next;
    cand_admitted |= 1u << k;
    memset(snap_size[k], 0, sizeof snap_size[k]);       /* (if a player that left had that seat: not the newcomer's) */
    msg_append(0, entry, 18);
    for (kind = 0; kind < SNAP_GLOBALS; kind++)
        if (snap_global_size[kind]) {
            entry[0] = J_SNAP;
            memcpy(entry + 1, snap_global[kind], snap_global_size[kind]);
            msg_append(snap_global_who[kind], entry, 1 + snap_global_size[kind] > MSG_BYTES ? MSG_BYTES : 1 + snap_global_size[kind]);
        }
    for (i = 0; i < SEATS; i++)
        for (kind = 0; kind < SNAP_KINDS; kind++)
            if ((connected >> i & 1) && snap_size[i][kind]) {
                entry[0] = J_SNAP;
                memcpy(entry + 1, snap_data[i][kind], snap_size[i][kind]);
                msg_append(i, entry, 1 + snap_size[i][kind] > MSG_BYTES ? MSG_BYTES : 1 + snap_size[i][kind]);
            }
    entry[0] = J_SNAP_END;
    entry[1] = 1;
    entry[2] = (uint8_t)k;
    msg_append(0, entry, 10);
    net_say("net: player %d is let in at this change of map\n", k + 1);
}

/* An entry of the order that is this layer's own. 0: done with it, go on to the next; 1: it holds a message for
 * the game (`*inner`, `*inner_size`); 2: it has to wait, nothing after it may be acted on yet. */
static int join_entry(const uint8_t *entry, int size, const uint8_t **inner, int *inner_size)
{
    int k = size > 3 ? entry[3] : -1;

    switch (entry[1]) {
    case J_INFO:
        if (k == seat && seat > 0 && join_role == JR_NONE && size >= 1 + (int)sizeof join_info) {
            int i;
            memcpy(join_info, entry + 1, sizeof join_info);
            connected = join_info[4] | 1u << seat;
            /* (the session's players are no candidates, whatever this program took them for before it knew:
             * it may have seen the host's "GO" still standing in its block and heard a player only after) */
            candidate &= ~(uint32_t)join_info[4];
            lobby_done = 1;
            gone = join_info[5];
            for (i = 0; i < SEATS; i++)
                if (gone >> i & 1) {
                    memset(gone_block[i], 0, BLOCK);
                    memcpy(gone_block[i], join_info + 8 + i * 8, 8);
                    gone_block[i][8] = (uint8_t)i;
                    gone_block[i][9] = 0x80;
                    memset(gone_block[i] + 20, 0x80, 4);
                }
            join_role = JR_LOBBY;
            net_say("net: the game is in progress (seats %X); this player joins it at the next change of map\n", (unsigned)join_info[4]);
        }
        return 0;
    case J_ADMIT:
        if (k == seat) {
            if (join_role == JR_PARKED && size >= 19) {
                int i;
                memcpy(join_admit, entry + 1, 18);
                join_role = JR_SNAP;
                /* a player the host gave up meanwhile and this program never heard: it stands for this one too */
                for (i = 1; i < SEATS; i++)
                    if ((join_admit[16] >> i & 1) && i != seat && !(gone >> i & 1) && !streams[i].known) {
                        memset(gone_block[i], 0, BLOCK);
                        gone_block[i][8] = (uint8_t)i;
                        gone_block[i][9] = 0x80;
                        memset(gone_block[i] + 20, 0x80, 4);
                        gone |= 1u << i;
                    }
            }
            return 0;
        }
        if (k < 1 || k >= SEATS || !(candidate >> k & 1))
            return 0;
        /* The others are given one seat more, at a moment when no barrier of theirs counts the seats with one
         * too few or too many: a machine whose token for the barrier of this map's layouts is not in the order
         * before this entry, and that has not handed it over yet, is still in the barrier before; it goes on
         * when it has (it hands that token over as soon as it is through). */
        if (!(size > 4 && (entry[4] >> seat & 1)) && tokens_out <= 0)
            return 2;
        candidate &= ~(1u << k);
        joined |= 1u << k;
        if (gone >> k & 1) {
            gone &= ~(1u << k);         /* a seat whose player left: the newcomer plays it from here on */
            away[k].epoch = 0;
        } else {
            grow_pending |= 1u << k;
        }
        net_say("net: player %d joined the game in progress\n", k + 1);
        return 0;
    case J_SNAP:
        if (join_role == JR_SNAP && size >= 4) {
            *inner = entry + 2;
            *inner_size = size - 2;
            return 1;
        }
        return 0;
    case J_SNAP_END:
        if (join_role == JR_SNAP && k == seat) {
            join_role = JR_BRINGUP;
            net_say("net: this player has what it missed; its game goes on into the map\n");
        }
        return 0;
    }
    return 0;
}

/* Once a frame, with the game's own block of this frame. */
static void join_frame(const uint8_t *own_block)
{
    if (seat == 0) {
        if (!in_play && own_block[8] == 'G' && own_block[9] == 'O')
            lobby_byte = own_block[10];         /* the lobby's settings, for whoever joins later */
        if (cand_owed)
            join_tell();
    }
}

/* The game asks, once per frame from its receiver tasks, for which seat it must make a player now (-1: none). */
int ss_net_join_grow(void)
{
    int k;
    if (configured != 1 || !grow_pending)
        return -1;
    for (k = 1; k < SEATS; k++)
        if (grow_pending >> k & 1)
            break;
    grow_pending &= ~(1u << k);
    return k;
}

/* The game says that it is (1) or is no longer (0) at the point of a map's set-up where the host announces the
 * layouts: the one place where a player is let in. */
void ss_net_join_anchor(int on)
{
    join_anchor = on;
}

/* The newcomer's game asks, after the start of its session, where the game in progress is: 1 and where[0..3] =
 * map, area, the way the exit gave, the callback column; 0 in every other program. From here on it is a
 * player like the others: its barriers are real (the game's function that selects the map ends in the barrier
 * the others stand in), and it is first given again the tokens the host held when it let it in. */
int ss_net_join_place(int *where)
{
    uint16_t half;
    uint32_t word;
    if (configured != 1 || join_role != JR_BRINGUP)
        return 0;
    memcpy(&half, join_admit + 4, 2);
    where[0] = half;
    memcpy(&word, join_admit + 8, 4);
    where[1] = (int)word;
    memcpy(&word, join_admit + 12, 4);
    where[2] = (int)word;
    memcpy(&half, join_admit + 6, 2);
    where[3] = half;
    net_say("net: this player enters map %d, area %d\n", where[0], where[1]);
    join_prefill = join_admit[3] & ~(1u << seat);
    {
        /* Tokens of the barriers behind it may still wait in the game's own queue of messages (games/ss2: 50
         * entries of 0x88 bytes at the address in gUnk_03006300, their number in gUnk_03006338, the tag in an
         * entry's first byte); the game's sender hands them over later. They are for nobody, like the ones
         * before. */
        uint32_t queue = peek32(0x03006300u), waiting = peek32(0x03006338u), i;
        join_drop_tokens = 0;
        for (i = 0; i < waiting && i < 50 && queue >= SS_EWRAM && queue < SS_EWRAM + 0x40000 - 50 * 0x88; i++)
            join_drop_tokens += *(const uint8_t *)(uintptr_t)(queue + i * 0x88) == 0x84;
    }
    join_role = JR_LIVE;
    join_first_init = 1;
    return 1;
}

/* 1 once, in the newcomer's first set-up of a map: the barrier and the announcement of the layouts that open a
 * set-up are behind the others already (the newcomer stands where the host stood when it let it in). */
int ss_net_join_first_init(void)
{
    if (!join_first_init)
        return 0;
    join_first_init = 0;
    return 1;
}

/* Is a session online, so that the game's messages go through this channel? It stays true while the session
 * waits for the host or for a seat: the messages wait too, and both ends of the game keep to one protocol. */
int ss_net_msg_online(void)
{
    return configured == 1;
}

/* Did that player leave the game in progress (its character stays, see drop)? It will send nothing more. */
int ss_net_msg_gone(int who)
{
    /* (a newcomer that has been given what it missed passes the barriers of its session's start without
     * anybody: the others passed theirs long ago; see "joining a game in progress") */
    return configured == 1 && who >= 0 && who < SEATS && ((gone >> who & 1) || join_role == JR_BRINGUP);
}

/* Did that player leave for good? Its character stays, and it cannot come back: it said goodbye, its program
 * ended, or it has been silent for longer than a player waits for its own connection. A player that was only
 * given up for silence may still come back (see "a connection that was gone"): where the game needs every
 * player's choice (the world map, games/<game>/port/patches.json) it is waited for, as the game waits for
 * anybody; one that left for good is passed over, or the others would wait for the rest of the session. */
int ss_net_msg_left(int who)
{
    return ss_net_msg_gone(who) && !away[who].epoch;
}

/* The game has just passed one of its barriers without the players in `seats` (a bit per seat): they left or
 * were given up, so ss_net_msg_gone counted them as ready. A barrier stands where everybody has to be at the
 * same point before the game goes on: the start of a dungeon from the world map, a change of level. A player
 * that was not there cannot be taken in again afterwards (see "a connection that was gone"); the host decides
 * that, as it decides everything about who is in the session. */
void ss_net_msg_passed(unsigned seats)
{
    int i;
    if (configured != 1 || seat != 0)
        return;
    for (i = 1; i < SEATS; i++)
        if ((seats >> i & 1) && (gone >> i & 1) && away[i].epoch) {
            away[i].epoch = 0;
            net_say("net: player %d was away when the game went on behind a barrier; it cannot come back\n", i + 1);
        }
}

/* One whole message of this player, for everybody. 0 if there is no room: the caller tries again next frame. */
int ss_net_msg_send(const void *data, int size)
{
    if (configured != 1 || !synced || seat < 0 || size < 1 || size > MSG_BYTES || !msg_room())
        return 0;
    if (join_mode && ((const uint8_t *)data)[0] == 0x84) {
        if (join_role != JR_NONE && join_role != JR_LIVE) {
            /* A newcomer's game before it is let in: the barrier tokens of its session's start are for nobody
             * (the others' games do not know it yet). The first one says that its game has reached the
             * barrier it waits at. */
            if (join_role == JR_LOBBY) {
                join_role = JR_PARKED;
                net_say("net: this player's game is ready; waiting for the others to change map\n");
            }
            in_play = 1;
            return 1;
        }
        if (join_drop_tokens > 0) {
            join_drop_tokens--;         /* (see ss_net_join_place) */
            return 1;
        }
        tokens_out++;
    } else if (join_role != JR_NONE && join_role != JR_LIVE) {
        /* Whatever else a newcomer's game says before it is let in is kept for the others only if it says who
         * this player is (its stream holds it until then); nothing else it could say now is about the game
         * the others are playing. */
        int i, keep = 0;
        for (i = 0; i < SNAP_KINDS; i++)
            keep |= ((const uint8_t *)data)[0] == snap_kind[i];
        in_play = 1;
        if (!keep)
            return 1;
    }
    if (join_mode && seat == 0 && join_anchor && ((const uint8_t *)data)[0] == 0x85)
        join_admit_now();
    msg_append(seat, data, size);
    msg_note('S', seat, data, size);
    in_play = 1;
    return 1;
}

/* The next entry of the order that the game has not been given, or NULL. */
static const uint8_t *msg_head(int *size)
{
    if (configured != 1 || seat < 0)
        return NULL;
    if (seat == 0) {
        if (out_own == out_next)
            return NULL;
        *size = out_size[out_own % MSG_OUT];
        return out_data[out_own % MSG_OUT];
    }
    if (streams[0].in_tail == streams[0].in_head)
        return NULL;
    *size = streams[0].in_size[streams[0].in_tail % MSG_IN];
    return streams[0].in_data[streams[0].in_tail % MSG_IN];
}

/* The next message of the order, whoever sent it (this player included): its size, 0 if none is here yet.
 * `who` is given the sender's seat. Every machine is given the same messages in the same order. */
int ss_net_msg_next(int *who, void *data, int capacity)
{
    int size = 0;
    const uint8_t *entry, *message;

    for (;;) {
        if (join_mode) {
            /* joining a game in progress: a seat the game has to make a player for comes first; a newcomer
             * that has what it missed is given nothing more until its game has selected the map; then, before
             * anything else, the barrier tokens the host held when it let it in */
            if (grow_pending || join_role == JR_BRINGUP)
                return 0;
            if (join_prefill && capacity >= 10) {
                static const uint8_t token[10] = { 0x84, 1, 0x64, 0x75, 0x03, 0x00, 0, 0, 0, 0 };
                int i;
                for (i = 0; !(join_prefill >> i & 1); i++)
                    ;
                join_prefill &= ~(1u << i);
                *who = i;
                memcpy(data, token, sizeof token);
                if (msg_log) {
                    fprintf(msg_log, "%u %u P %d 84\n", (unsigned)ss_plat_ticks_ms(), (unsigned)frame_label, i);
                    fflush(msg_log);
                }
                return (int)sizeof token;
            }
        }
        entry = msg_head(&size);
        if (!entry || size < 2 || entry[0] >= SEATS)
            return 0;
        message = entry + 1;
        size--;
        if (join_mode && entry[1] >= J_INFO && entry[1] <= J_SNAP_END) {
            int what = join_entry(entry, size + 1, &message, &size);
            if (what == 2)
                return 0;
            if (msg_log) {      /* (fewer fields than a message's line: the tools that compare messages pass over it) */
                fprintf(msg_log, "%u %u J %d %02X\n", (unsigned)ss_plat_ticks_ms(), (unsigned)frame_label, entry[0], entry[1]);
                fflush(msg_log);
            }
            if (what == 0) {
                if (seat == 0)
                    out_own++;
                else
                    streams[0].in_tail++;
                continue;
            }
        }
        break;
    }
    *who = entry[0];
    if (size > capacity)
        size = capacity;
    memcpy(data, message, (size_t)size);
    if (seat == 0)
        out_own++;
    else
        streams[0].in_tail++;
    if (join_mode && *who == seat && message[0] == 0x84 && message == entry + 1 && tokens_out > 0)
        tokens_out--;
    if (message == entry + 1)
        msg_note('R', *who, message, size);
    in_play = 1;
    return size;
}

/* The same for a caller that only takes the messages of player `who`: 0 while the next one is another's. */
int ss_net_msg_recv(int who, void *data, int capacity)
{
    int size = 0, sender;
    const uint8_t *entry = msg_head(&size);

    if (!entry || entry[0] != who)
        return 0;
    return ss_net_msg_next(&sender, data, capacity);
}

/* The message part of a packet from seat `from` (s is its stream). `mine` says the sender knows my current
 * stream: only then do its acknowledgement and its starting number mean this stream. */
static void receive_messages(struct stream *s, int from, const uint8_t *part, int size, int mine)
{
    const uint8_t *entry;
    uint32_t number, start, acked;
    int carried, at = MSG_HEAD, i;

    if (size < MSG_HEAD || part[0] != MSG_MARK || seat < 0)
        return;
    if (seat != 0 && from != 0)
        return;                 /* two players that are not the host have nothing to tell each other here */
    /* the entries are for every seat but the sender's own, in order */
    entry = part + 6 + (seat - (seat > from)) * 8;
    start = rd32(entry);
    acked = rd32(entry + 4);
    if (!mine || !start)
        return;                 /* it does not know me yet: its stream is not for me */
    if (acked > s->msg_acked && acked <= out_next)
        s->msg_acked = acked;
    if (!s->in_valid || start > s->in_next) {
        /* the first word from it, or it gave me up for a while and starts again further on */
        s->in_valid = 1;
        s->in_next = start;
    }
    carried = part[1];
    number = rd32(part + 2);
    for (i = 0; i < carried && at < size; i++, number++) {
        int length = part[at];
        if (at + 1 + length > size || length > MSG_ENTRY)
            break;
        /* in order only: a later one is repeated until the ones before it are here; with no room nothing is
         * taken, so the sender repeats until there is */
        if (number == s->in_next && length >= 2) {
            if (seat == 0 && (candidate >> from & 1) && !(cand_admitted >> from & 1)) {
                /* a candidate's messages wait in its own stream until it is let in: the others' games have no
                 * player for them yet */
            } else if (seat == 0) {
                /* the host: this is the moment the message gets its place in the order */
                if (msg_room()) {
                    msg_append(from, part + at + 2, length - 1);
                    s->in_next++;
                    relay_fresh = 1;
                }
            } else if (join_mode && join_role == JR_NONE && length >= 4 && part[at + 2] == J_INFO && part[at + 4] == seat) {
                /* The host tells this program that it is joining a game in progress. Taken here and not by
                 * the game, which has no task yet that takes messages. */
                const uint8_t *inner;
                int inner_size;
                join_entry(part + at + 1, length, &inner, &inner_size);
                s->in_next++;
            } else if (join_mode && (join_role == JR_LOBBY || join_role == JR_PARKED) && !join_admit_seen
                       && !(length >= 4 && part[at + 2] == J_ADMIT && part[at + 4] == seat)) {
                /* a candidate takes nothing of the order before the entry that lets it in: what the others
                 * play now is not for its game (the host moves the start of its stream along) */
            } else if (s->in_head - s->in_tail < MSG_IN) {
                if (join_mode && (join_role == JR_LOBBY || join_role == JR_PARKED))
                    join_admit_seen = 1;
                s->in_size[s->in_head % MSG_IN] = (uint8_t)length;
                memcpy(s->in_data[s->in_head % MSG_IN], part + at + 1, (size_t)length);
                s->in_head++;
                s->in_next++;
            }
        }
        at += 1 + length;
    }
}

static void receive(void)
{
    uint8_t packet[PACKET_MAX + 64];
    int size, i;

    while ((size = ss_plat_net_recv(packet, sizeof packet)) >= HEADER) {
        int type = packet[4], from = packet[5], count = packet[7];
        struct stream *s;
        uint32_t first, label;

        if (rd32(packet) != MAGIC || rd32(packet + 8) != room)
            continue;
        relay_heard = ss_plat_ticks_ms();       /* whatever it is, the connection is there (connection_check) */
        if (type == T_WELCOME) {
            if (from < SEATS && seat != from) {
                seat = from;
                net_say("net: seat %d of room '%s'\n", seat + 1, room_name);
                start_session();
            }
            continue;
        }
        if (from >= SEATS || from == seat || seat < 0)
            continue;
        if (gone >> from & 1) {
            /* It left the game; whoever sits there now is not part of it. Unless it is the stream that seat had
             * when it fell silent: then its connection was gone and is back (revive). A goodbye of that stream
             * says its program ended meanwhile. */
            if (type == T_END && rd32(packet + 16) == away[from].epoch)
                away[from].epoch = 0;
            if (type == T_DATA && size >= HEADER + count * BLOCK && away[from].epoch && rd32(packet + 16) == away[from].epoch)
                revive(from, rd32(packet + 20), rd32(packet + 24));
            else if (!join_mode || (type == T_DATA && rd32(packet + 16) == old_epoch[from]))
                continue;
            /* (with SS_NET_JOIN another program on that seat is a candidate for it: it goes on below) */
        }
        s = &streams[from];
        if (type == T_END) {
            if (s->known && s->epoch == rd32(packet + 16)) {
                old_epoch[from] = s->epoch;     /* a packet of it that arrives after the goodbye is not a return */
                drop(from, "left", 0);
            }
            continue;
        }
        if (type != T_DATA || size < HEADER + count * BLOCK)
            continue;
        if (rd32(packet + 16) == old_epoch[from])
            continue;           /* a late packet of the stream it had before: taking it for a new one would
                                 * restart that player, and with it the numbering of its messages */
        if (!s->known || s->epoch != rd32(packet + 16)) {       /* a new stream of that seat */
            old_epoch[from] = s->known ? s->epoch : 0;
            if (s->known)
                drop(from, "restarted", 0);
            s->known = 1;
            s->epoch = rd32(packet + 16);
            s->first = rd32(packet + 20);
            s->floor = s->first;
            if (synced && frame_label - delay > s->floor)
                s->floor = frame_label - delay;                 /* it was here before me, or I before it */
            s->next = s->floor;
            s->cursor = 0;
            s->has_last = s->repeats = 0;
            s->msg_start = s->msg_acked = out_next;     /* it was not here for what I sent before */
            if (join_mode && (in_play || lobby_done) && (!(connected >> from & 1) || (gone >> from & 1))) {
                /* a program started while the game is in progress: a candidate until the host lets it in
                 * ("joining a game in progress") */
                candidate |= 1u << from;
                cand_told &= ~(1u << from);
                cand_ready &= ~(1u << from);
                cand_admitted &= ~(1u << from);
                if (seat == 0)
                    cand_owed |= 1u << from;
            }
            net_say("net: player %d connected\n", from + 1);
        }
        s->heard = ss_plat_ticks_ms();
        if (seat == 0 && (candidate >> from & 1) && (packet[6] & JOIN_PARKED))
            cand_ready |= 1u << from;   /* its game waits at its first barrier: it can be let in */
        if (rd32(packet + 44 + seat * 4) == epoch)
            s->acked = rd32(packet + 28 + seat * 4);
        if (from == 0 && synced) {
            /* does the host count this player in? Once it has named this player's stream it goes on doing so
             * for as long as the player is part of its session (see "a connection that was gone") */
            if (rd32(packet + 44 + seat * 4) == epoch) {
                host_knew = 1;
                disowned_since = 0;
            } else if (host_knew && !disowned_since) {
                disowned_since = s->heard | 1;
            }
        }
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
            receive_messages(s, from, trailer + TRAILER, size - (HEADER + count * BLOCK + TRAILER),
                             rd32(packet + 44 + seat * 4) == epoch);
        }
        first = rd32(packet + 24);
        if (from == 0 && !synced && first > s->next)
            s->next = first;            /* not on the timeline yet: any recent block of the host tells where it is */
        if (direct && count == MAX_BLOCKS && first > s->next) {
            /* A hole. A packet starts at the oldest block any receiver lacks, but never more than MAX_BLOCKS
             * back; this one is full and still starts after the block waited for, and every later one will
             * start later still. So the blocks in between will never come: the stream was silent for longer
             * than a packet reaches back (a connection that was gone). They are stepped over. What they held
             * of the player's state is out of date, and in a game in progress their message bytes say nothing
             * (the messages have their own channel). Without this the stream stood still for good: the game
             * was given that player's last block before the hole for the rest of the session. */
            s->floor = s->next = first;
        }
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
            delay = (packet[6] & 0x3F) ? (packet[6] & 0x3F) : delay;
            frame_label = own_first = s->next;
            last_target = 0;
            epoch = fnv(epoch + 1, &s->heard, 4) | 1;
            msg_reset();
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
            net_say("net: joined the host at frame %u, delay %d\n", (unsigned)frame_label, delay);
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
    msg_reset();
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
        if (i != seat && streams[i].known && !(candidate >> i & 1)) {
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
    if (session_open) {
        /* The game starts its link task again without ever having left the others (it does on some changes of
         * place). On a cable nothing is seen of that: the same consoles go on. Here too: the same session
         * continues, and the goodbye that the task's end had prepared is not said. */
        if (leaving_since)
            net_say("net: the game's link task started again; the session goes on\n");
        leaving_since = 0;
        return;
    }
    leave();
    start_session();
    send_hello();
    session_open = 1;
}

/* The link task ended its session (called from the game through games/<game>/port/patches.json). */
void ss_net_leave(void)
{
    last_sendp = NULL;
    /* Not said to the others yet: if the game starts its link task again in a moment, nobody left. A player
     * the others are told has left stays for them as a standing character for the rest of the game, so the
     * goodbye waits two seconds (ss_net_flush says it then; closing the program says it at once). */
    if (session_open && !leaving_since)
        leaving_since = ss_plat_ticks_ms() | 1;
}

/* End of a frame of the game (called from VBlankIntrWait). The game has just written its block for this frame;
 * the link task would only pass it to MultiSioMain in the next frame, after the picture has been shown and the
 * frame's time waited out. Sending it now takes that frame off everything the other players see of this one,
 * and off every message handed over during the frame. The next MultiSioMain call finds its block already sent. */
void ss_net_flush(void)
{
    if (leaving_since && ss_plat_ticks_ms() - leaving_since > 2000) {
        net_say("net: the game's link task ended; leaving the session\n");
        leaving_since = 0;
        session_open = 0;
        leave();
    }
    if (configured != 1 || !synced || !last_sendp || flushed_label == frame_label)
        return;
    memcpy(own[frame_label % RING], last_sendp, BLOCK);
    send_data();
    flushed_label = frame_label;
}

/* ---- direct mode ----
 * The game is not a simulation every machine must step in unison: each machine decides for its own character
 * and tells the others (bytes 0..7 of the block: position, facing, animation, rewritten every frame), and
 * everything that happens once (an enemy appears, an item is dropped or taken, a hit, a change of map) is a
 * message. Over a cable the game passes a message step by step in bytes 8..23, each step repeated until every
 * player's block answers it (games/ss2: sub_08066720 and sub_0806690C); online it travels whole through the
 * message channel above. So nothing has to arrive on a particular frame, and the cable never promised that
 * either. No player waits for another.
 *
 * Per player the blocks are handed over in order, one a frame, the moment they are here. Whatever is passed
 * step by step in bytes 8..23 must be seen in order (the lobby's handshakes, and the messages if a game's
 * sender is not patched), so a block is never skipped for being late: it is asked for again (the
 * acknowledgements in every packet do that). When several are waiting because they came in a burst, the ones
 * that only repeat the message bytes of the block before them are dropped, which is how the game catches up
 * without missing a step. When nothing new has come the last block is given again, as the unchanged state it
 * is, for up to DIRECT_HOLD frames before the game is told that player is silent (not during a game in
 * progress, see below). A player's own block is in its own place at once. */
static uint32_t deliver_direct(void *sendp, void *recvp)
{
    static uint32_t logged_quarter;
    uint32_t received = 0, now = ss_plat_ticks_ms();
    int i;
    for (i = 0; i < SEATS; i++) {
        struct stream *s = &streams[i];
        const uint8_t *block = NULL;
        if (i == seat) {
            block = sendp;
        } else if (s->known) {
            if (s->cursor < s->floor) s->cursor = s->floor;
            /* catch up: a waiting block that says nothing new in its message bytes, with a newer one behind it */
            while (s->has_last && s->next > s->cursor + 1
                   && !memcmp(s->block[s->cursor % RING] + 8, s->last + 8, BLOCK - 8))
                s->cursor++;
            if (s->cursor < s->next) {
                memcpy(s->last, s->block[s->cursor % RING], BLOCK);
                s->cursor++;
                s->has_last = 1;
                s->repeats = 0;
                block = s->last;
            } else if (s->has_last && (s->repeats < DIRECT_HOLD || in_play)) {
                /* in a game in progress the last block stands for as long as the player is in the room: told
                 * that it is silent, the game would end the session for everybody after 80 frames; this way
                 * a pause of the connection only freezes that character until it is back or given up (drop) */
                s->repeats++;
                block = s->last;
            }
            if (!offline_since && now - s->heard > patience(i))
                drop(i, "timed out", 1);
        }
        if (join_mode && i == 0 && block && !lobby_done && block[8] == 'G' && block[9] == 'O') {
            lobby_done = 1;     /* from here on a program that turns up is one that joins a game in progress */
            net_say("net: the lobby is over; whoever comes now joins a game in progress\n");
        }
        if ((candidate | grow_pending) >> i & 1)
            block = NULL;       /* it wants to join the game in progress: the game does not know it yet */
        if (gone >> i & 1) {
            block = gone_block[i];
            /* A player that is itself cut off waits SS_NET_AWAY for its connection and then gives the session
             * up. One that has been silent for that long after it was given up here will not come back. */
            /* `since` is read from the clock in drop(), which this same pass may just have called: it can be a
             * millisecond later than `now`. Compared as unsigned that was "away for 49 days", and the player
             * was written off the moment it timed out (found on Linux, 0.8; a matter of timing everywhere). */
            if (away[i].epoch && (int32_t)(now - away[i].since) > away_ms) {
                away[i].epoch = 0;
                net_say("net: player %d has been away for %u seconds; it is not expected back\n", i + 1,
                        (unsigned)((now - away[i].since + patience(i)) / 1000));
            }
        }
        if (join_role != JR_NONE && !in_play && i != seat && (connected >> i & 1)) {
            /* This program is joining a game in progress and its game is in the lobby: the others are played
             * as the lobby wants them, the host saying "GO" with the settings it had, everybody else "OK"
             * (games/ss2: the lobby reads bytes 8 to 10 of the blocks). Their state bytes are the real ones. */
            static uint8_t lobby[SEATS][BLOCK];
            memset(lobby[i], 0, BLOCK);
            if (block)
                memcpy(lobby[i], block, 8);
            lobby[i][8] = i == 0 ? 'G' : 'O';
            lobby[i][9] = i == 0 ? 'O' : 'K';
            lobby[i][10] = i == 0 ? join_info[3] : 0;
            block = lobby[i];
        } else if (join_role != JR_NONE && !block && i != seat && (connected >> i & 1)) {
            /* A seat the session counts and this program, which came later, has never heard (its player may
             * be gone, or about to be given up by the others): the game is given an idle block for it. Told
             * that a counted seat is silent, the game would end the session here after 80 frames. */
            static uint8_t idle[SEATS][BLOCK];
            idle[i][8] = (uint8_t)i;
            idle[i][9] = 0x80;
            memset(idle[i] + 20, 0x80, 4);
            block = idle[i];
        }
        if (block) {
            memcpy((uint8_t *)recvp + i * BLOCK, block, BLOCK);
            received |= 1u << i;
        }
    }
    if (log_file && now / 250 != logged_quarter) {
        /* SS_NET_LOG with this transport (tests): four times a second, at the same moments of the clock on
         * every machine of one computer, the state bytes (0..7) of the block the game was just given for each
         * seat, "-" for a seat it was given nothing for. What one player's program has of another can then be
         * held against what that player has of itself: a character that stays frozen after its connection came
         * back shows here. tools/net_compare.py passes over these lines (they have more than three fields). */
        logged_quarter = now / 250;
        fprintf(log_file, "Q %u %u %X", (unsigned)logged_quarter, (unsigned)frame_label, (unsigned)gone);
        for (i = 0; i < SEATS; i++) {
            const uint8_t *given = (const uint8_t *)recvp + i * BLOCK;
            if (received >> i & 1)
                fprintf(log_file, " %02X%02X%02X%02X%02X%02X%02X%02X", given[0], given[1], given[2], given[3], given[4],
                        given[5], given[6], given[7]);
            else
                fprintf(log_file, " -");
        }
        fprintf(log_file, "\n");
        fflush(log_file);
    }
    frame_label++;
    connected |= received;
    if (connected && available < 8)
        available++;
    return received | (seat == 0 ? 0x80 : 0) | connected << 8 | (available >> 3) << 15;
}

/* The relay is told that this player is here: HELLO every two seconds, which also lets the relay follow a
 * player whose address changed; four times a second while it has no seat; twice a second while nothing at all
 * is heard, so that a connection that comes back is noticed, and known to the relay again, at once. Then: is
 * this player's own connection gone (connection_check)? */
static void keep_in_touch(uint32_t now)
{
    uint32_t every = seat < 0 ? 250 : relay_heard && now - relay_heard > 700 ? 500 : 2000;
    if (now - last_hello > every)
        send_hello();
    connection_check(now);
}

/* The host's packets arrive and have left this player's stream out for seconds: it gave this player up and
 * cannot or will not take it in again (see "a connection that was gone"). The game is told what it is told when
 * the host leaves. Called every frame, and while a frame waits for blocks: on the stepped transport a player
 * that comes back waits for a block of the host that will never come (the host went on without it), and
 * waited there, the program standing still, for as long as the host's session lasted. */
static void disowned_check(uint32_t now)
{
    uint8_t packet[HEADER];
    if (seat <= 0 || !synced || !in_play || !disowned_since || (int32_t)(now - disowned_since) <= DISOWNED_AFTER)
        return;
    disowned_since = 0;
    /* The other players may have taken this one in again (they need nothing from the host for that): they
     * are told that it leaves after all, so its character stands for them as it does for the host. */
    header(packet, T_END, 0, 0);
    ss_plat_net_send(packet, HEADER);
    ss_plat_net_send(packet, HEADER);
    drop(0, "has given this player up (it was away for too long); the session is over here", 0);
}

/* MultiSioMain: one block out, the blocks of frame F - delay in. */
static uint32_t multi_sio_main(void *sendp, void *recvp);
uint32_t __wrap_sub_08001804(void *sendp, void *recvp)
{
    uint32_t result;
    ss_prof(SS_PROF_NET, 1);
    result = multi_sio_main(sendp, recvp);
    ss_prof(SS_PROF_NET, 0);
    return result;
}

static uint32_t multi_sio_main(void *sendp, void *recvp)
{
    uint32_t now, target, received = 0, waited_since, hash = 2166136261u;
    int i, told = 0, waited = 0;

    if (configured != 1)
        return __real_sub_08001804(sendp, recvp);
    now = ss_plat_ticks_ms();
    {
        /* If this program itself stood still (its window was being moved, the machine was busy, it was
         * suspended), the others were not silent: it was not listening. Their clocks start again, or every one
         * of them would be given up at once and stay a standing character for the rest of the game. */
        static uint32_t last_poll;
        if (last_poll && now - last_poll > 1000) {
            net_say("net: this program stood still for %u ms; nobody is counted as silent for that\n", (unsigned)(now - last_poll));
            for (i = 0; i < SEATS; i++)
                if (streams[i].known)
                    streams[i].heard = now;
        }
        last_poll = now;
    }
    receive();
    now = ss_plat_ticks_ms();   /* not the moment before receive(): what was just heard must not lie in the future */
    keep_in_touch(now);
    disowned_check(now);
    if (relay_fresh) {
        /* A player's message has just been given its place in the order. The block of this frame went out at
         * the end of the last one (ss_net_flush), so nothing would be sent until the end of this one: say it
         * now, the others are waiting for it. */
        relay_fresh = 0;
        if (synced && flushed_label == frame_label)
            send_data();
    }
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
    {
        /* the window's title says who is being waited for: a player that has been silent for two seconds */
        int silent = -1;
        for (i = SEATS - 1; i >= 0; i--)
            if (i != seat && streams[i].known && now - streams[i].heard > 2000)
                silent = i;
        status(silent);
    }
    if (!synced && host_lost)
        return host_lost << 8 | 0x1000;         /* see drop: the parent is gone */
    if (!synced)
        return 0;               /* a console with no parent on the cable hears nothing, itself included */

    if (farewell_epoch != epoch) {          /* closing the window says goodbye too */
        uint8_t packet[HEADER];
        header(packet, T_END, 0, 0);
        ss_plat_net_farewell(packet, HEADER);
        farewell_epoch = epoch;
    }
    last_sendp = sendp;
    if (join_mode)
        join_frame(sendp);
    test_chatter();
    test_message();
    if (flushed_label != frame_label) {         /* not already sent at the end of the frame that wrote it */
        memcpy(own[frame_label % RING], sendp, BLOCK);
        send_data();
    }
    if (direct)
        return deliver_direct(sendp, recvp);
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
        net_say("net: delay %d -> %d frames\n", delay, new_delay);
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
            net_say("net: waiting for player %d\n", missing + 1);
            status(missing);
        }
        keep_in_touch(now);
        disowned_check(now);
        if (!synced)
            return 0;
        for (i = 0; i < SEATS; i++)
            if (!offline_since && streams[i].known && now - streams[i].heard > patience(i))
                drop(i, "timed out", 1);
    }
    if (waited)
        stall_frames += (int)(ss_plat_ticks_ms() - waited_since);       /* milliseconds waited in this window */
    for (i = 0; i < SEATS; i++) {
        const uint8_t *block = NULL;
        if (i == seat)
            block = target >= own_first ? own[target % RING] : sendp;
        else if (gone >> i & 1)
            block = gone_block[i];
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
