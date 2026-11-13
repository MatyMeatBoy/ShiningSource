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
 * wait at the start of the session until n players are in the room), SS_NET_MSG_LOG=file (the message channel,
 * see "messages" below: one line per message handed over and per message dispatched).
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
enum { MSG_BYTES = 2 + 0x80, MSG_ENTRY = 1 + MSG_BYTES, MSG_OUT = 1024, MSG_IN = 128, MSG_MARK = 0x5A,
       MSG_HEAD = 6 + (SEATS - 1) * 8 };
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
 * understood afterwards from what each player's program saw. The file is small: a line per event. */
static void net_say(const char *format, ...)
{
    char text[200];
    va_list arguments;
    FILE *file;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    fprintf(stderr, "%s", text);
    if (getenv("SS_FRAMES"))
        return;                 /* test runs keep their own logs */
    if ((file = fopen("shiningsource_net.log", "ab")) != NULL) {
        fprintf(file, "%10u ms  frame %lu  seat %d  %s", (unsigned)ss_plat_ticks_ms(), (unsigned long)ss_frame_number(), seat + 1, text);
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
        timeout_ms = (int)strtoul(getenv("SS_NET_TIMEOUT"), NULL, 10);
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
         * it speaks again). Another player's stream is for the host alone. */
        uint8_t *part = packet + size, *entry = part + 6;
        uint32_t first = out_next, number, now = ss_plat_ticks_ms();
        int carried = 0;
        for (i = 0; i < SEATS; i++) {
            struct stream *s = &streams[i];
            if (i == seat)
                continue;
            wr32(entry, s->known ? s->msg_start : 0);
            wr32(entry + 4, s->known && s->in_valid ? s->in_next : 0);
            entry += 8;
            if (s->known && (seat == 0 ? now - s->heard < 500 : i == 0) && s->msg_acked < first)
                first = s->msg_acked;
        }
        if (first < out_base)
            first = out_base;
        size += MSG_HEAD;
        for (number = first; number != out_next && carried < 255; number++, carried++) {
            int length = out_size[number % MSG_OUT];
            if (size + 1 + length > PACKET_MAX)
                break;
            packet[size] = (uint8_t)length;
            memcpy(packet + size + 1, out_data[number % MSG_OUT], (size_t)length);
            size += 1 + length;
        }
        part[0] = MSG_MARK;
        part[1] = (uint8_t)carried;
        wr32(part + 2, first);
    }
    ss_plat_net_send(packet, size);
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
 * passes over it (ss_net_msg_gone, used from games/<game>/port/patches.json). A stream that later shows up on that seat is ignored until this player's
 * own session ends: the game has no way to let somebody in halfway.
 *
 * Seat 0 is the exception. It is the game's parent: only it decides enemies and items, and the seat numbers
 * cannot change during a game, so nobody can take that over. When the host leaves a game in progress the others
 * are told what a cable tells when the parent console is pulled out: everybody still counted, nobody heard, and
 * the driver's error bit. The game's link task then runs into its own link error handling. */
static void drop(int who, const char *why)
{
    struct stream *s = &streams[who];
    /* the block it was last seen with: the one handed over last (direct), or the newest received (stepped) */
    const uint8_t *seen = s->has_last ? s->last : s->next > s->floor ? s->block[(s->next - 1) % RING] : NULL;
    int stays = in_play && who != 0 && who != seat && s->known && seen && (connected & 1u << who);

    if (s->known)
        net_say("net: player %d %s%s\n", who + 1, why, stays ? " (its character stays)" : "");
    if (stays) {
        memcpy(gone_block[who], seen, BLOCK);
        gone_block[who][8] = (uint8_t)who;      /* the message bytes as a sender leaves them between messages */
        gone_block[who][9] = 0x80;
        memset(gone_block[who] + 10, 0, 10);
        memset(gone_block[who] + 20, 0x80, 4);  /* and as a receiver leaves them while it waits */
        gone |= 1u << who;
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
    if (who == 0 && seat > 0 && in_play && s->known)
        host_lost = connected;
    memset(s, 0, sizeof *s);
    connected &= ~(1u << who);
    if (who == 0 && seat > 0)
        synced = 0;             /* the timeline is the host's */
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
        fprintf(msg_log, "%u %u %c %d %02X %d\n", (unsigned)ss_plat_ticks_ms(), (unsigned)frame_label, what, who,
                size > 0 ? data[0] : 0, size);
        fflush(msg_log);
    }
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
            if (streams[i].known && streams[i].msg_acked < have)
                have = streams[i].msg_acked;
        if (out_own < have)
            have = out_own;
    } else if (streams[0].known) {
        have = streams[0].msg_acked;
    } else {
        have = out_base;        /* no host to take them */
    }
    if (have > out_base)
        out_base = have;
    return out_next - out_base < MSG_OUT;
}

/* Put a message of seat `who` at the end of my stream. */
static void msg_append(int who, const uint8_t *data, int size)
{
    uint8_t *entry = out_data[out_next % MSG_OUT];
    entry[0] = (uint8_t)who;
    memcpy(entry + 1, data, (size_t)size);
    out_size[out_next % MSG_OUT] = (uint8_t)(1 + size);
    out_next++;
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
    return configured == 1 && who >= 0 && who < SEATS && (gone >> who & 1);
}

/* One whole message of this player, for everybody. 0 if there is no room: the caller tries again next frame. */
int ss_net_msg_send(const void *data, int size)
{
    if (configured != 1 || !synced || seat < 0 || size < 1 || size > MSG_BYTES || !msg_room())
        return 0;
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
    const uint8_t *entry = msg_head(&size);

    if (!entry || size < 2 || entry[0] >= SEATS)
        return 0;
    if (seat == 0)
        out_own++;
    else
        streams[0].in_tail++;
    *who = entry[0];
    size--;
    if (size > capacity)
        size = capacity;
    memcpy(data, entry + 1, (size_t)size);
    msg_note('R', *who, entry + 1, size);
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
            if (seat == 0) {
                /* the host: this is the moment the message gets its place in the order */
                if (msg_room()) {
                    msg_append(from, part + at + 2, length - 1);
                    s->in_next++;
                    relay_fresh = 1;
                }
            } else if (s->in_head - s->in_tail < MSG_IN) {
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
        if (gone >> from & 1)
            continue;           /* it left the game; whoever sits there now is not part of it */
        s = &streams[from];
        if (type == T_END) {
            if (s->known && s->epoch == rd32(packet + 16)) {
                old_epoch[from] = s->epoch;     /* a packet of it that arrives after the goodbye is not a return */
                drop(from, "left");
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
                drop(from, "restarted");
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
            net_say("net: player %d connected\n", from + 1);
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
            receive_messages(s, from, trailer + TRAILER, size - (HEADER + count * BLOCK + TRAILER),
                             rd32(packet + 44 + seat * 4) == epoch);
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
            if (now - s->heard > (uint32_t)timeout_ms)
                drop(i, "timed out");
        }
        if (gone >> i & 1)
            block = gone_block[i];
        if (block) {
            memcpy((uint8_t *)recvp + i * BLOCK, block, BLOCK);
            received |= 1u << i;
        }
    }
    frame_label++;
    connected |= received;
    if (connected && available < 8)
        available++;
    return received | (seat == 0 ? 0x80 : 0) | connected << 8 | (available >> 3) << 15;
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
    status(-1);
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
