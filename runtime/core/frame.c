/* Frame boundary: what happens when the game waits for the vertical blank.
 *
 * The cartridge's interrupt dispatcher (IntrMain in crt0.s) looks the pending interrupt up in a table of 14
 * handlers that the game fills. Here the table is read directly and the handler is a native function.
 *
 * Test controls (environment):
 *   SS_FRAMES=N     run N frames without a window, print frame hashes, exit (SS_PACE=1: at 60 frames a second)
 *   SS_QUIT=N       with a window: end the program at frame N (timed test runs; SS_STATS=1 prints the measurements)
 *   SS_TEST_SAVE=N  at frame N call the game's save routine for the current character and print its result
 *   SS_DUMP=N       at frame N write ss_video_dump.bin (registers, palette, VRAM, OAM) and ss_frame.raw (rendered)
 *   SS_INPUT=...    scripted buttons (SS_INPUT_FILE=path reads the script from a file): "300:0001,310:0000,..." = from frame 300 hold mask 0x0001 (A), then release.
 *                   Mask bits: A B Select Start Right Left Up Down R L.
 *   SS_INPUT_SHIFT=F:N  from frame F on, the script is read N frames ahead (online tests: every player gets its own
 *                   N, so the players do not all press the same buttons and stand on one spot)
 *   SS_INPUT_HUNT=F  from frame F on, while an enemy lives, the script's buttons are replaced: walk to the nearest
 *                   enemy and strike it (games/ss2; online tests, so that enemies die and come back). The
 *                   player's seat varies the rhythm, so the players do not act as one.
 *   SS_INPUT_EXIT=F  from frame F on, once no enemy lives and the map's exit is open, the script's buttons are
 *                   replaced: walk to the nearest exit (games/ss2; online tests of a change of map; with
 *                   SS_INPUT_HUNT the players first clear the map).
 *   SS_INPUT_GOTO=F:X:Y[:T]  from frame F on (up to frame T), while no enemy lives, the script's direction buttons
 *                   are replaced: walk to the point X,Y of the map (pixels), first across, then along
 *                   (games/ss2; online tests).
 *   SS_STATE_LOG=file  where the game is (mode, scene flags, map, area, seats, the players' states), a line per
 *                   change: see state_log below.
 *   SS_ENEMY_LOG=file  four times a second, at the same moments of the clock on every machine, write the state of
 *                   every live enemy (games/ss2: the five records at gUnk_030056F4). tools/enemy_compare.py
 *                   compares the files of the players of one session.
 *   SS_SCENE_LOG=file  the same moments: the map the game is on and the tokens its barrier holds of each player
 *                   (games/ss2). tools/net_outage_report.py compares the files of the players of one session.
 *   SS_WHERE_LOG=file  every tenth frame: map, area or room of the town, where this player's character stands, its
 *                   flags and the scene's (games/ss2). tools/town_report.py reads it for the town tests.
 *   SS_TEST_WARP=F:M:A  at frame F this player's game changes to area A of map set M (games/ss2; 0:0 is the town)
 *   SS_PACE_FAST=F:K  with SS_PACE: the first F frames at K times the speed.
 *   SS_PROF=1       time the stages of a frame (game, sound, network, picture, presenting) and print mean, median,
 *                   99th percentile and maximum per stage when the program ends; SS_PROF=F:T counts only game
 *                   frames F to T. Off, the cost is one test per stage.
 *   SS_TEST_RENDER=W  without a window (SS_FRAMES): draw and finish every frame W columns wide all the same, as a
 *                   window of that width would, and print one hash over all those pictures at the end. Measures
 *                   the picture's cost at full speed and proves that two builds draw the same.
 *   SS_BENCH=F:N    picture tests without a window (tools/tests/flicker_bench.py): from frame F - 40 on every frame
 *                   is drawn, and frames F .. F+N-1 are written to ss_bench.bin (SS_BENCH_OUT=path). With
 *                   SS_BENCH_SCRIPT=path test sprites are put into the sprite table for the picture only.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG16(offset) (*(volatile uint16_t *)(uintptr_t)(SS_IO + (offset)))
#define REG_DISPSTAT REG16(0x004)
#define REG_VCOUNT   REG16(0x006)
#define REG_KEYINPUT REG16(0x130)
#define REG_IE       REG16(0x200)
#define REG_IF       REG16(0x202)
#define REG_IME      REG16(0x208)

/* order used by the dispatcher */
static const uint16_t dispatch_order[14] = {
    0x0004, 0x0002, 0x00C0, 0x0800, 0x0100, 0x0001, 0x0008, 0x0010, 0x0020, 0x0040, 0x0200, 0x0400, 0x1000, 0x2000,
};
static uint32_t *const handler_table = (uint32_t *)(uintptr_t)0x030012A0u;

static unsigned long frame_count, frame_limit, dump_at;
static int test_render;                 /* SS_TEST_RENDER: the picture's width, 0 if not asked */
static uint32_t picture_hash = 2166136261u;
static unsigned long picture_count;
static uint32_t frame_pixels[SS_MAX_VIEW_W * SS_SCREEN_H];      /* the picture of the frame at hand, with or without a window */
static const char *input_script;
static int windowed, forced_width;

unsigned long ss_frame_number(void)
{
    return frame_count;
}

uint32_t ss_frame_hash(void)
{
    static const struct { uint32_t base, size; } parts[] = {
        /* video registers only: the DMA registers further up hold native addresses, which differ per build */
        { SS_IO, 0x56 }, { SS_PALETTE, 0x400 }, { SS_VRAM, 0x18000 }, { SS_OAM, 0x400 },
    };
    uint32_t hash = 2166136261u;
    unsigned i, j;
    for (i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)parts[i].base;
        for (j = 0; j < parts[i].size; j++)
            hash = (hash ^ bytes[j]) * 16777619u;
    }
    return hash;
}

/* ---- stage timer (SS_PROF, see the top of this file) ----
 * Stages nest (the sound and the link run inside the game's part of a frame): a stage's time is its own, without
 * what ran inside it. Times are kept per frame in a histogram of 16 steps to the octave, so a percentile is
 * good to about 3%. The clock is the processor's counter; its rate is taken from the milliseconds of the run. */
enum { PROF_STEPS = 16 * 44 };
static int prof_on = -1;
static unsigned long prof_from, prof_to = ~0ul;
static uint64_t prof_frame[SS_PROF_STAGES], prof_sum[SS_PROF_STAGES], prof_most[SS_PROF_STAGES], prof_first_tick, prof_last_tick;
static unsigned long prof_most_at[SS_PROF_STAGES];
static uint32_t prof_count[SS_PROF_STAGES], prof_steps[SS_PROF_STAGES][PROF_STEPS], prof_first_ms, prof_last_ms;
static struct { int stage; uint64_t from, inner; } prof_open[8];
static int prof_depth;

static uint64_t prof_clock(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static int prof_step(uint64_t value)
{
    int octave = 4, step;
    if (value < 16)
        return (int)value;
    while (octave < 63 && (value >> (octave + 1)))
        octave++;
    step = (octave - 3) * 16 + (int)((value >> (octave - 4)) & 15);
    return step < PROF_STEPS ? step : PROF_STEPS - 1;
}

static double prof_value(int step)
{
    int octave = step / 16 + 3;
    return step < 16 ? (double)step : (double)((uint64_t)(16 + step % 16) << (octave - 4)) + (double)((uint64_t)1 << (octave - 4)) / 2;
}

static void prof_report(void)
{
    static const char *const names[SS_PROF_STAGES] = { "game logic", "sound (m4aSoundMain)", "link (flush, MultiSioMain)", "blank handlers, lines",
                                                       "cheats, achievements", "picture (ss_ppu_render)", "  of it find_flicker", "finish_picture",
                                                       "present (upload, draw, wait)" };
    double per_us = prof_last_ms != prof_first_ms ? (double)(prof_last_tick - prof_first_tick) / ((double)(prof_last_ms - prof_first_ms) * 1000.0) : 0;
    int stage, step;
    if (per_us <= 0)
        return;
    fprintf(stderr, "prof: frames %lu to %lu, %u clock ticks a microsecond; microseconds per frame, share of 16743%c", prof_from,
            frame_count, (unsigned)per_us, 10);
    for (stage = 0; stage < SS_PROF_STAGES; stage++) {
        uint32_t seen = 0, n = prof_count[stage];
        double p50 = 0, p99 = 0, mean;
        if (!n)
            continue;
        for (step = 0; step < PROF_STEPS; step++) {
            uint32_t before = seen;
            seen += prof_steps[stage][step];
            if (before <= n / 2 && seen > n / 2) p50 = prof_value(step) / per_us;
            if (before <= n - 1 - n / 100 && seen > n - 1 - n / 100) p99 = prof_value(step) / per_us;
        }
        mean = (double)prof_sum[stage] / n / per_us;
        /* the image's own printf has no floating point: tenths of a microsecond as two whole numbers */
        fprintf(stderr, "prof: %s: n %u  mean %u.%u  p50 %u.%u  p99 %u.%u  max %u.%u (frame %lu)  share %u.%02u%%%c", names[stage], (unsigned)n,
                (unsigned)mean, (unsigned)(mean * 10) % 10, (unsigned)p50, (unsigned)(p50 * 10) % 10, (unsigned)p99, (unsigned)(p99 * 10) % 10,
                (unsigned)((double)prof_most[stage] / per_us), (unsigned)((double)prof_most[stage] / per_us * 10) % 10, prof_most_at[stage],
                (unsigned)(mean * 100.0 / 16743.0), (unsigned)(mean * 10000.0 / 16743.0) % 100, 10);
    }
}

void ss_prof(int stage, int begin)
{
    uint64_t now;
    if (prof_on <= 0) {
        const char *text, *colon;
        if (!prof_on)
            return;
        text = getenv("SS_PROF");
        prof_on = text && *text && *text != '0';
        if (!prof_on)
            return;
        colon = text;
        while (*colon && *colon != ':')
            colon++;
        if (*colon == ':') {
            prof_from = strtoul(text, NULL, 10);
            prof_to = strtoul(colon + 1, NULL, 10);
        }
        atexit(prof_report);
    }
    now = prof_clock();
    if (begin) {
        if (prof_depth < 8) {
            prof_open[prof_depth].stage = stage;
            prof_open[prof_depth].from = now;
            prof_open[prof_depth].inner = 0;
        }
        prof_depth++;
    } else if (prof_depth > 0 && --prof_depth < 8 && prof_open[prof_depth].stage == stage) {
        uint64_t whole = now - prof_open[prof_depth].from;
        prof_frame[stage] += whole - prof_open[prof_depth].inner;
        if (prof_depth > 0)
            prof_open[prof_depth - 1].inner += whole;
    }
}

/* The frame is over: its stage times go to the histograms. */
static void prof_frame_end(unsigned long frame)
{
    int stage;
    if (prof_on <= 0)
        return;
    if (frame >= prof_from && frame <= prof_to) {
        prof_last_tick = prof_clock();
        prof_last_ms = ss_plat_ticks_ms();
        if (!prof_first_tick) {
            prof_first_tick = prof_last_tick;
            prof_first_ms = prof_last_ms;
        } else {
            for (stage = 0; stage < SS_PROF_STAGES; stage++) {
                uint64_t took = prof_frame[stage];
                if (!took && stage != SS_PROF_LOGIC)
                    continue;           /* a stage that did not run in this frame */
                prof_count[stage]++;
                prof_sum[stage] += took;
                prof_steps[stage][prof_step(took)]++;
                if (took > prof_most[stage]) {
                    prof_most[stage] = took;
                    prof_most_at[stage] = frame;
                }
            }
        }
    }
    memset(prof_frame, 0, sizeof prof_frame);
}

void ss_raise_interrupt(uint16_t flag)
{
    unsigned i;
    if (!(REG_IME & 1) || !(REG_IE & flag))
        return;
    REG_IF |= flag;
    for (i = 0; i < 14; i++) {
        if (dispatch_order[i] & flag) {
            uint32_t handler = handler_table[i];
            if (handler)
                ((void (*)(void))(uintptr_t)handler)();
            return;
        }
    }
}

/* Sound of one game frame (48 kHz stereo). It is measured, optionally written to ss_audio.raw
 * (SS_AUDIO_FILE), and handed to the platform, which plays it when there is a device. */
static unsigned long audio_frames, audio_loud;
static int audio_peak;
static FILE *audio_file;

static int skip_mute;           /* a skipped scene makes no sound */
static int skip_fast;           /* a scene is being skipped (tests without a window pace themselves by it) */

void ss_audio_push(const int16_t *samples, int frames)
{
    static int file_checked;
    int i;
    if (skip_mute)
        return;
    for (i = 0; i < frames * 2; i++) {
        int magnitude = samples[i] < 0 ? -samples[i] : samples[i];
        if (magnitude > audio_peak) audio_peak = magnitude;
        if (magnitude > 256) audio_loud++;
    }
    audio_frames += (unsigned long)frames;
    if (!file_checked) {
        file_checked = 1;
        if (getenv("SS_AUDIO_FILE"))
            audio_file = fopen("ss_audio.raw", "wb");
    }
    if (audio_file)
        fwrite(samples, 4, (size_t)frames, audio_file);
    if (windowed)
        ss_plat_audio(samples, frames);
}

void ss_audio_report(void)
{
    fprintf(stderr, "audio: %lu sample frames, peak %d, %lu samples above the noise floor%c", audio_frames, audio_peak, audio_loud, 10);
    if (audio_file)
        fclose(audio_file);
}

static uint16_t script_extra;       /* the extra buttons the script holds (core/qol.c), SS_QOL_BARS=1 only */
enum { EXITS = 8 };
static int exit_known[EXITS];
static long exit_at[EXITS][2];      /* SS_INPUT_EXIT: the exits of the current map, in pixels */

static void scripted_input(void)
{
    static unsigned long shift_from = ~0ul, shift_by;
    const char *cursor = input_script;
    unsigned long at = frame_count;
    uint16_t held = 0;
    if (shift_from == ~0ul) {           /* SS_INPUT_SHIFT=F:N, see the top of this file */
        const char *text = getenv("SS_INPUT_SHIFT");
        char *end = NULL;
        shift_from = text ? strtoul(text, &end, 10) : 0;
        shift_by = end && *end == ':' ? strtoul(end + 1, NULL, 10) : 0;
    }
    if (shift_by && at >= shift_from)
        at += shift_by;
    while (cursor && *cursor) {
        char *end;
        unsigned long from = strtoul(cursor, &end, 10), mask;
        if (*end != ':')
            break;
        mask = strtoul(end + 1, &end, 16);
        if (from <= at)
            held = (uint16_t)mask;
        cursor = *end == ',' ? end + 1 : NULL;
    }
    {   /* SS_INPUT_HUNT=F, see the top of this file. Positions are those of games/ss2: the players' records at
         * gUnk_03003CE0 (0x14C bytes each, x and y at 0x8C and 0x90), the enemies' at gUnk_030056F4 (0x2E0 bytes
         * each, x and y at 0x14 and 0x18, live ones in the mask gUnk_030056FC), all in 1/256 pixel. */
        static unsigned long hunt_from = ~0ul;
        if (hunt_from == ~0ul)
            hunt_from = getenv("SS_INPUT_HUNT") ? strtoul(getenv("SS_INPUT_HUNT"), NULL, 10) : 0;
        if (hunt_from && frame_count >= hunt_from) {
            uint32_t records = *(const uint32_t *)(uintptr_t)0x030056F4u, mask = *(const uint32_t *)(uintptr_t)0x030056FCu & 31;
            int me = REG16(0x128) >> 4 & 3, i, best = -1;
            int32_t mine[2], theirs[2], dx = 0, dy = 0;
            long nearest = 0;
            memcpy(mine, (const uint8_t *)(uintptr_t)(0x03003CE0u + (uint32_t)me * 0x14C + 0x8C), 8);
            if (records < SS_EWRAM || records >= SS_EWRAM + 0x40000 - 5 * 0x2E0)
                mask = 0;
            for (i = 0; i < 5; i++)
                if (mask >> i & 1) {
                    long far;
                    memcpy(theirs, (const uint8_t *)(uintptr_t)(records + (uint32_t)i * 0x2E0 + 0x14), 8);
                    {       /* the game image's C library has no labs */
                        long ax = (long)(theirs[0] - mine[0]) >> 8, ay = (long)(theirs[1] - mine[1]) >> 8;
                        far = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay);
                    }
                    if (best < 0 || far < nearest) {
                        best = i;
                        nearest = far;
                        dx = (theirs[0] - mine[0]) >> 8;
                        dy = (theirs[1] - mine[1]) >> 8;
                    }
                }
            if (best >= 0) {
                unsigned long beat = (frame_count + (unsigned long)me * 7) % (unsigned long)(20 + me * 3);
                /* stand beside it, on the left or the right by seat, turn to it, strike */
                static int reach = -1;  /* SS_INPUT_HUNT_REACH=pixels: how far beside the enemy to stand (20) */
                int side;
                if (reach < 0)
                    reach = getenv("SS_INPUT_HUNT_REACH") ? (int)strtoul(getenv("SS_INPUT_HUNT_REACH"), NULL, 10) : 20;
                side = me & 1 ? reach : -reach;
                dx += side;
                held = 0;
                if (dx > 4) held |= 0x10;               /* right */
                if (dx < -4) held |= 0x20;              /* left */
                if (dy > 4) held |= 0x80;               /* down */
                if (dy < -4) held |= 0x40;              /* up */
                if (!held && beat < 3)
                    held = side < 0 ? 0x10 : 0x20;      /* in place: face it */
                else if (beat >= 6 && beat < 10)
                    held = (uint16_t)((held & 0xF0) | 1);       /* a strike; it also moves on the talk some
                                                                 * enemies start with */
            }
        }
    }
    {   /* SS_INPUT_EXIT=F, see the top of this file. The exits are those the game's exit tasks named
         * (ss_test_exit_note); the scene's flags are the halfword at +4 of the record gUnk_03005CF4 points to
         * (bit 3: the exit is open, bit 4: the change of map is under way). */
        static unsigned long exit_from = ~0ul;
        if (exit_from == ~0ul)
            exit_from = getenv("SS_INPUT_EXIT") ? strtoul(getenv("SS_INPUT_EXIT"), NULL, 10) : 0;
        if (exit_from && frame_count >= exit_from) {
            uint32_t scene = *(const uint32_t *)(uintptr_t)0x03005CF4u, live = *(const uint32_t *)(uintptr_t)0x030056FCu & 31;
            uint16_t flags = 0;
            int me = REG16(0x128) >> 4 & 3, i, best = -1;
            int32_t mine[2];
            long nearest = 0;
            if (scene >= SS_EWRAM && scene < SS_EWRAM + 0x40000 - 8)
                memcpy(&flags, (const uint8_t *)(uintptr_t)(scene + 4), 2);
            if (flags & 0x10) {
                memset(exit_known, 0, sizeof exit_known);      /* the map goes: its exits with it */
                held = 0;
            }
            memcpy(mine, (const uint8_t *)(uintptr_t)(0x03003CE0u + (uint32_t)me * 0x14C + 0x8C), 8);
            for (i = 0; i < EXITS; i++)
                if (exit_known[i]) {
                    long ax = exit_at[i][0] - (mine[0] >> 8), ay = exit_at[i][1] - (mine[1] >> 8);
                    long far = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay);
                    if (best < 0 || far < nearest) {
                        best = i;
                        nearest = far;
                    }
                }
            if (best >= 0 && (flags & 8) && !(flags & 0x10) && !live) {
                long dx = exit_at[best][0] - (mine[0] >> 8), dy = exit_at[best][1] - (mine[1] >> 8);
                /* a step aside now and then: the way is not always free */
                unsigned long beat = frame_count % 240;
                held = 0;
                if (dx > 3) held |= 0x10;
                if (dx < -3) held |= 0x20;
                if (dy > 3) held |= 0x80;
                if (dy < -3) held |= 0x40;
                if (held && beat >= 200)
                    held = (uint16_t)(beat < 220 ? 0x40 : 0x10);
            }
        }
    }
    {   /* SS_INPUT_GOTO=F:X:Y, see the top of this file */
        static unsigned long goto_from = ~0ul, goto_until = ~0ul;
        static long goto_x, goto_y;
        if (goto_from == ~0ul) {
            const char *text = getenv("SS_INPUT_GOTO");
            char *end = NULL;
            goto_from = text ? strtoul(text, &end, 10) : 0;
            goto_x = end && *end == ':' ? (long)strtoul(end + 1, &end, 10) : 0;
            goto_y = end && *end == ':' ? (long)strtoul(end + 1, &end, 10) : 0;
            if (end && *end == ':')
                goto_until = strtoul(end + 1, &end, 10);
        }
        if (goto_from && frame_count >= goto_from && frame_count < goto_until && !(*(const uint32_t *)(uintptr_t)0x030056FCu & 31)) {
            int me = REG16(0x128) >> 4 & 3;
            int32_t mine[2];
            long dx, dy;
            memcpy(mine, (const uint8_t *)(uintptr_t)(0x03003CE0u + (uint32_t)me * 0x14C + 0x8C), 8);
            dx = goto_x - (mine[0] >> 8);
            dy = goto_y - (mine[1] >> 8);
            held &= 0x030F;             /* the script's other buttons stay (A moves a dialogue on) */
            /* across first, then along: the ways of a map are not straight lines */
            if (dx > 3) held |= 0x10;
            else if (dx < -3) held |= 0x20;
            else if (dy > 3) held |= 0x80;
            else if (dy < -3) held |= 0x40;
        }
    }
    REG_KEYINPUT = (uint16_t)(~held & 0x03FF);
    script_extra = ss_qol_scripted(held);
}

/* Called by the game's exit task when it starts (games/ss2/port/patches.json): where an exit of the current map
 * is, in pixels. Only kept for SS_INPUT_EXIT; nothing in the game depends on it. */
void ss_test_exit_note(int slot, int x, int y)
{
    if (slot < 0 || slot >= EXITS)
        return;
    exit_known[slot] = 1;
    exit_at[slot][0] = x;
    exit_at[slot][1] = y;
    if (getenv("SS_INPUT_EXIT"))
        fprintf(stderr, "test: frame %lu exit %d at %d,%d\n", frame_count, slot, x, y);
}

/* SS_STATE_LOG=file (tests): where this machine's game is, one line whenever it changes and at least once a
 * second: the clock in quarters of a second, the frame, the game's mode flags (gUnk_03003478), the scene's
 * flags (the halfword at +4 of the record gUnk_03005CF4 points to), the map set and the area in it
 * (gUnk_03003170 + 0x8C and + 0x90), the number of players the game counts (the connected bits of the link
 * status word gUnk_03002730), then per player record (gUnk_03003CE0, 0x14C bytes each) its state (+0x20), its
 * flags (+0x30) and its position in pixels (+0x8C, +0x90). Offsets are those of games/ss2. A player that joined
 * a game in progress is in the same place as the others when these lines agree (tools/net_join_report.py). */
static void state_log(void)
{
    static FILE *file;
    static int asked;
    static char last[200];
    static unsigned long last_frame;
    char line[200];
    uint32_t scene = *(const uint32_t *)(uintptr_t)0x03005CF4u, status = *(const uint32_t *)(uintptr_t)0x03002730u;
    uint16_t scene_flags = 0xFFFF, level, area;
    int i, at;

    if (!asked) {
        asked = 1;
        if (getenv("SS_STATE_LOG"))
            file = fopen(getenv("SS_STATE_LOG"), "w");
    }
    if (!file)
        return;
    if (scene >= SS_EWRAM && scene < SS_EWRAM + 0x40000 - 8)
        memcpy(&scene_flags, (const uint8_t *)(uintptr_t)(scene + 4), 2);
    memset(line, 0, sizeof line);       /* (compared whole below: the game image's C library has no strcmp) */
    memcpy(&level, (const uint8_t *)(uintptr_t)(0x03003170u + 0x8C), 2);
    memcpy(&area, (const uint8_t *)(uintptr_t)(0x03003170u + 0x90), 2);
    at = snprintf(line, sizeof line, "%08X %04X %u %u %X |", (unsigned)*(const uint32_t *)(uintptr_t)0x03003478u, scene_flags,
                  level, area, (unsigned)(status >> 8 & 15));
    for (i = 0; i < 4 && at < (int)sizeof line - 40; i++) {
        const uint8_t *player = (const uint8_t *)(uintptr_t)(0x03003CE0u + (uint32_t)i * 0x14C);
        uint16_t state;
        uint32_t flags;
        int32_t where[2];
        memcpy(&state, player + 0x20, 2);
        memcpy(&flags, player + 0x30, 4);
        memcpy(where, player + 0x8C, 8);
        at += snprintf(line + at, sizeof line - (size_t)at, " %u %X %d %d", state, (unsigned)flags, (int)(where[0] >> 12), (int)(where[1] >> 12));
    }
    /* (positions are compared in steps of 16 pixels, so a walking player does not write a line a frame) */
    if (!memcmp(line, last, sizeof last) && frame_count - last_frame < 60)
        return;
    memcpy(last, line, sizeof last);
    last_frame = frame_count;
    fprintf(file, "%u %lu %s\n", (unsigned)(ss_plat_ticks_ms() / 250), frame_count, line);
    fflush(file);
}

/* SS_ENEMY_LOG (tests): the live enemies as this machine has them, sampled when the clock passes a quarter of a
 * second, so the players of a session on one computer sample within a frame of each other. One line per
 * sample: the quarter's number, the frame, the mask of live records and of spawn requests under way, then per
 * live enemy its record, the identifier its spawn gave it, its kind (index of its definition in the ROM's
 * table), position in pixels, health and state. Offsets are those of games/ss2 (sub_0801641C fills them). */
static void enemy_log(void)
{
    static FILE *file;
    static int asked;
    static uint32_t last_quarter;
    uint32_t quarter = ss_plat_ticks_ms() / 250, records, mask;
    int i;

    if (!asked) {
        asked = 1;
        if (getenv("SS_ENEMY_LOG"))
            file = fopen(getenv("SS_ENEMY_LOG"), "w");
    }
    if (!file || quarter == last_quarter)
        return;
    last_quarter = quarter;
    records = *(const uint32_t *)(uintptr_t)0x030056F4u;
    mask = *(const uint32_t *)(uintptr_t)0x030056FCu & 31;
    if (records < SS_EWRAM || records >= SS_EWRAM + 0x40000 - 5 * 0x2E0)
        return;                         /* no enemy records yet */
    fprintf(file, "%u %lu %X %X", (unsigned)quarter, frame_count, (unsigned)mask,
            (unsigned)(*(const uint16_t *)(uintptr_t)0x03005724u & 3));
    for (i = 0; i < 5; i++)
        if (mask >> i & 1) {
            const uint8_t *e = (const uint8_t *)(uintptr_t)(records + (uint32_t)i * 0x2E0);
            uint32_t definition;
            int32_t x, y;
            uint16_t id, state, health;
            memcpy(&id, e + 2, 2);
            memcpy(&state, e + 0xC, 2);
            memcpy(&x, e + 0x14, 4);
            memcpy(&y, e + 0x18, 4);
            memcpy(&health, e + 0x5C, 2);
            memcpy(&definition, e + 0x2DC, 4);
            fprintf(file, " | %d %u %d %d %d %u %u", i, id, (int)((definition - 0x0847AD3Cu) / 0xD8), (int)(x >> 8), (int)(y >> 8),
                    health, state);
        }
    fprintf(file, "%c", 10);
    fflush(file);
}

/* SS_TEST_WARP=F:MAP:AREA (tests, games/ss2): at frame F this player's game leaves its map for area AREA of map
 * set MAP, with the game's own steps for a change of map (what its "everybody changes map" message does on
 * every machine: sub_0800D12C, sub_08056FB8, sub_08057170). A single-player test reaches the town with it
 * (map set 0; area 0 is the square behind the gate) without playing the first field to its end; online the
 * message itself is used (SS_NET_TEST_MSG). It runs as a task of the game, because the steps wait. */
void sub_0800D12C(void);
void sub_08056FB8(uint16_t map, uint16_t area);
void sub_08057170(void);
uint32_t sub_08000274(uint32_t, uint32_t, uint32_t);    /* start a task */
static uint16_t warp_map, warp_area;

static void test_warp_task(void)
{
    sub_0800D12C();
    sub_08056FB8(warp_map, warp_area);
    sub_08057170();
}

static void test_warp(void)
{
    static long at = -1;
    static uint32_t record[10];
    if (at < 0) {
        const char *text = getenv("SS_TEST_WARP");
        char *end;
        at = 0;
        if (text) {
            at = (long)strtoul(text, &end, 10);
            warp_map = *end == ':' ? (uint16_t)strtoul(end + 1, &end, 10) : 0;
            warp_area = *end == ':' ? (uint16_t)strtoul(end + 1, &end, 10) : 0;
        }
    }
    if (at && frame_count == (unsigned long)at) {
        memset(record, 0, sizeof record);
        record[2] = 0x50;
        record[3] = (uint32_t)(uintptr_t)test_warp_task;
        fprintf(stderr, "warp test: map %u area %u asked at frame %lu, result %u%c", warp_map, warp_area, frame_count,
                (unsigned)sub_08000274((uint32_t)(uintptr_t)record, 0, 0), 10);
    }
}

/* SS_WHERE_LOG=file (tests, games/ss2): every tenth frame one line: the frame, the scene's map set and area
 * (in the town, map set 0, the room the player is in, which the scene keeps at +2), this player's seat, where
 * its character stands (pixels) and its flags, the scene's flags (bit 11: an event holds the player), then
 * the bag: how many of its records are used and the money in it. A route written as buttons is checked
 * against it: did the walk reach the room and the counter, did a sale or a purchase change the bag, and did
 * the player get the control back. Offsets as for SS_SCENE_LOG; the players' records are 0x14C bytes each at
 * gUnk_03003CE0 (position at +0x8C/+0x90 in 1/256 pixel, flags at +0x30); the bag is 25 records of 16
 * bytes at +0x150 of the character's shared block *gUnk_030034E0 (u16 kind, 10 is money; u32 amount at +4). */
uint32_t sub_080020F4(void);            /* which seat is this machine's */

static void where_log(void)
{
    static FILE *file;
    static int asked;
    uint32_t scene, area, seat, shared, used = 0, purse = 0;
    const uint8_t *player;
    uint16_t map;
    int32_t x, y;
    uint32_t flags;

    if (!asked) {
        asked = 1;
        if (getenv("SS_WHERE_LOG"))
            file = fopen(getenv("SS_WHERE_LOG"), "w");
    }
    if (!file || frame_count % 10)
        return;
    scene = *(const uint32_t *)(uintptr_t)0x03005CF4u;
    if (scene < SS_EWRAM || scene >= SS_EWRAM + 0x40000 - 0x44)
        return;                         /* no scene yet */
    memcpy(&map, (const uint8_t *)(uintptr_t)scene, 2);
    memcpy(&area, (const uint8_t *)(uintptr_t)(scene + 0x40), 4);
    if (map == 0) {                     /* the town's rooms are numbered at +2 */
        uint16_t room;
        memcpy(&room, (const uint8_t *)(uintptr_t)(scene + 2), 2);
        area = room;
    }
    seat = sub_080020F4() & 3;
    player = (const uint8_t *)(uintptr_t)(0x03003CE0u + seat * 0x14C);
    memcpy(&x, player + 0x8C, 4);
    memcpy(&y, player + 0x90, 4);
    memcpy(&flags, player + 0x30, 4);
    shared = *(const uint32_t *)(uintptr_t)0x030034E0u;
    if (shared >= SS_EWRAM && shared <= SS_EWRAM + 0x40000 - 0xCA4) {
        int i;
        for (i = 0; i < 25; i++) {
            const uint8_t *record = (const uint8_t *)(uintptr_t)(shared + 0x150 + (uint32_t)i * 16);
            uint16_t kind;
            uint32_t amount;
            memcpy(&kind, record, 2);
            memcpy(&amount, record + 4, 4);
            used += kind != 0;
            if (kind == 10)
                purse += amount;
        }
    }
    fprintf(file, "%lu %u %u %u %d %d %X %X %u %u%c", frame_count, map, (unsigned)area, (unsigned)seat, (int)(x >> 8), (int)(y >> 8),
            (unsigned)flags, (unsigned)*(const uint32_t *)(uintptr_t)0x03003478u, (unsigned)used, (unsigned)purse, 10);
    fflush(file);
}

/* SS_SCENE_LOG (tests): where this machine's game is, sampled like SS_ENEMY_LOG. One line per sample: the
 * quarter's number, the frame, the scene's map set and area, the scene's flags and whether the world map is
 * being chosen from, then for each of the four seats how many tokens the game's barrier holds of it and the
 * oldest of them. Offsets are those of games/ss2: the scene record at *gUnk_03005CF4 (map set at +0, flags at
 * +4, area at +0x40), the world scene's flags at gUnk_03003478 (bit 5: choosing on the world map), the rings
 * of barrier tokens at gUnk_03006260 (ten words a seat, 0xFFFFFFFF where empty; sub_080682B8 reads them).
 * tools/net_outage_report.py holds the players of one session against each other: the same place for all,
 * and no token left over once a change of map is done. */
static void scene_log(void)
{
    static FILE *file;
    static int asked;
    static uint32_t last_quarter;
    uint32_t quarter = ss_plat_ticks_ms() / 250, scene, area, flags;
    uint16_t map, state;
    int i, j;

    if (!asked) {
        asked = 1;
        if (getenv("SS_SCENE_LOG"))
            file = fopen(getenv("SS_SCENE_LOG"), "w");
    }
    if (!file || quarter == last_quarter)
        return;
    last_quarter = quarter;
    scene = *(const uint32_t *)(uintptr_t)0x03005CF4u;
    if (scene < SS_EWRAM || scene >= SS_EWRAM + 0x40000 - 0x44)
        return;                         /* no scene yet */
    memcpy(&map, (const uint8_t *)(uintptr_t)scene, 2);
    memcpy(&state, (const uint8_t *)(uintptr_t)(scene + 4), 2);
    memcpy(&area, (const uint8_t *)(uintptr_t)(scene + 0x40), 4);
    flags = *(const uint32_t *)(uintptr_t)0x03003478u;
    fprintf(file, "%u %lu %u %u %X %u", (unsigned)quarter, frame_count, map, (unsigned)area, state, (unsigned)(flags >> 5 & 1));
    for (i = 0; i < 4; i++) {
        const uint32_t *ring = (const uint32_t *)(uintptr_t)(0x03006260u + (uint32_t)i * 40);
        int held = 0;
        for (j = 0; j < 10; j++)
            held += ring[j] != 0xFFFFFFFFu;
        fprintf(file, " %d:%X", held, (unsigned)ring[0]);
    }
    fprintf(file, "%c", 10);
    fflush(file);
}

static void dump_video(void)
{
    static uint32_t shot[SS_MAX_VIEW_W * SS_SCREEN_H];
    struct ss_view view;
    int width = forced_width ? forced_width : SS_SCREEN_W;
    FILE *file = fopen("ss_video_dump.bin", "wb");
    if (file) {
        fwrite((void *)(uintptr_t)SS_IO, 1, 0x400, file);
        fwrite((void *)(uintptr_t)SS_PALETTE, 1, 0x400, file);
        fwrite((void *)(uintptr_t)SS_VRAM, 1, 0x18000, file);
        fwrite((void *)(uintptr_t)SS_OAM, 1, 0x400, file);
        fclose(file);
    }
    file = fopen("ss_ram_dump.bin", "wb");          /* work RAM, to find where two builds first differ */
    if (file) {
        fwrite((void *)(uintptr_t)SS_IWRAM, 1, 0x8000, file);
        fwrite((void *)(uintptr_t)SS_EWRAM, 1, 0x40000, file);
        fclose(file);
    }
    file = fopen("ss_frame.raw", "wb");
    if (file) {
        if (!ss_field_view(&view, width)) {
            memset(&view, 0, sizeof view);
            view.width = width;
        }
        ss_ppu_render(shot, width, &view);
        fwrite(shot, 4, (size_t)width * SS_SCREEN_H, file);
        fclose(file);
    }
}

/* The picture's lines. The game can ask for an interrupt when the console reaches a line (the description that
 * scrolls across the character selection moves its background only for its own lines that way). The lines
 * are walked here, after the vertical-blank handler as on the console; the handler runs at its line and the
 * registers of every line are kept for the renderer. This runs with or without a window: the handler is game
 * code, and every player must run the same. */
static uint8_t line_registers[SS_SCREEN_H][SS_LINE_REGISTERS];
static int lines_vary;

const uint8_t *ss_line_registers(int y)
{
    return lines_vary ? line_registers[y] : NULL;
}

static void walk_lines(void)
{
    int y;
    lines_vary = 0;
    if (!(REG_DISPSTAT & 0x0020))
        return;
    for (y = 0; y < SS_SCREEN_H; y++) {
        REG_VCOUNT = (uint16_t)y;
        if ((REG_DISPSTAT & 0x0020) && (REG_DISPSTAT >> 8) == y) {
            REG_DISPSTAT |= 4;
            ss_raise_interrupt(0x0004);
            lines_vary = 1;
        } else {
            REG_DISPSTAT &= ~4;
        }
        memcpy(line_registers[y], (const void *)(uintptr_t)SS_IO, SS_LINE_REGISTERS);
    }
    REG_DISPSTAT &= ~4;
    REG_VCOUNT = 160;
}

/* ---- the finished picture, before it is shown ----
 *
 * Fast flicker. The cartridge makes things blink by changing them every frame or every two (the option being
 * confirmed in a menu, a selection that is redrawn, effects), counting on a slow screen to melt the two pictures
 * together; a modern display shows each one sharply and the eye gets a hard blink. Where a pixel goes A, B, A
 * (or A, A, B, B, A) it is shown as the mix of the two, steadily, for as long as it keeps doing that. A pixel
 * that simply changes is untouched. So is a pixel that showed something moving in any of the frames compared
 * (a background that scrolled, a sprite that changed its place; the renderer tells which): a fine pattern
 * that scrolls, or a small sprite passing, makes a pixel go A, B, A too, and mixing those blurs what moves.
 * Sprites that alternate are not this function's business: the renderer draws them steadily itself, also
 * while they move (see "sprites that alternate" in ppu.c). SS_SMOOTH=0 turns it off.
 *
 * The sides. In a scene that keeps the console's composition the columns beside it are left to the platform,
 * which draws its own field there. When the game's picture goes dark (a fade, a loading screen) a bright
 * frame around a black screen is the last thing wanted: the sides darken with the picture, a step at a time. */
static int smooth = -1;         /* not read yet */

/* The platform's options menu switches the smoothing while the game runs (1 on, 0 off). */
void ss_frame_set_smooth(int on)
{
    smooth = on != 0;
}

/* The same menu switches between the classic and the enhanced picture: the width every frame is given from now
 * on (as SS_VIEW), or 0 to follow the window. */
void ss_frame_set_view(int width)
{
    forced_width = !width ? 0 : width < SS_SCREEN_W ? SS_SCREEN_W : width > SS_MAX_VIEW_W ? SS_MAX_VIEW_W : width;
}

const uint8_t *ss_ppu_still(void);       /* core/ppu.c: per pixel of the last picture drawn, 1 where what it shows did not move */

static void finish_picture(uint32_t *pixels, int width)
{
    /* The last four pictures, as they were drawn. They are kept in a ring: the newest takes the place of the
     * oldest and `newest` moves on, so a frame costs one copy of the picture, not four. */
    static uint32_t past[4][SS_MAX_VIEW_W * SS_SCREEN_H];
    static uint8_t calm[SS_MAX_VIEW_W * SS_SCREEN_H];      /* for how many frames in a row the pixel has shown something that stayed in place */
    const uint8_t *restrict still = ss_ppu_still();
    static int past_width, shade, newest;
    int count = width * SS_SCREEN_H, i, wanted;
    unsigned long light = 0, sampled = 0;
    uint32_t *restrict now_at = pixels, *restrict oldest;
    const uint32_t *restrict a_at, *restrict b_at, *restrict c_at;

    if (smooth < 0)
        smooth = !(getenv("SS_SMOOTH") && getenv("SS_SMOOTH")[0] == '0');
    if (past_width != width) {          /* the window changed: nothing to compare with */
        for (i = 0; i < 4; i++)
            memcpy(past[i], pixels, (size_t)count * 4);
        memset(calm, 0, sizeof calm);
        past_width = width;
    }
    a_at = past[newest];
    b_at = past[(newest + 1) & 3];
    c_at = past[(newest + 2) & 3];
    oldest = past[(newest + 3) & 3];
    newest = (newest + 3) & 3;
    /* the brightness of the game's part of the picture, from every fourth pixel as drawn */
    for (i = 0; i < count; i += 4) {
        uint32_t now = pixels[i];
        if (now >> 24) {                /* not a side column: there is nothing of the game's there */
            light += ((now >> 16) & 255) + ((now >> 8) & 255) * 2 + (now & 255);
            sampled += 4;
        }
    }
    /* No branch in this loop, and no pointer shares memory with another: the compiler turns it into a few
     * instructions for several pixels at a time. Every pixel is written, most with its own value. A pixel is only
     * mixed where what it shows has stayed in place for the frames compared: something that moved is not blinking. */
    for (i = 0; i < count; i++) {
        uint32_t now = now_at[i], a = a_at[i], b = b_at[i], c = c_at[i], d = oldest[i], other, mixed;
        unsigned game = (now >> 24) != 0, was = calm[i];
        unsigned quiet = game ? (still[i] ? (was < 8 ? was + 1 : 8) : 0) : was;    /* a side column keeps its count */
        calm[i] = (uint8_t)quiet;
        /* B A B A: it has kept doing it; A B B A; A A B B A A */
        other = ((quiet >= 4) & (now != a) & (((now == b) & (a == c)) | ((a == b) & (now == c)))) ? a
                : ((quiet >= 5) & (now == a) & (now != b) & (b == c) & (now == d)) ? b : now;
        mixed = 0xFF000000u | (((now >> 1) & 0x7F7F7F) + ((other >> 1) & 0x7F7F7F) + (now & other & 0x010101));
        oldest[i] = now;
        now_at[i] = ((smooth != 0) & (other != now) & ((other >> 24) != 0) & game) ? mixed : now;
    }
    /* How dark the sides are follows how bright the game's picture is as a whole (not its brightest point, which
     * stays until the last moment of a fade): black when the picture is black, clear from a moderately lit
     * picture up, and never more than a small step from one frame to the next, so they sink and rise with the
     * game's own fade instead of switching. */
    {
        int mean = sampled ? (int)(light / sampled) : 0;
        wanted = mean >= 72 ? 0 : 255 - mean * 255 / 72;
    }
    if (wanted > shade) shade = wanted - shade > 8 ? shade + 8 : wanted;
    else shade = shade - wanted > 8 ? shade - 8 : wanted;
    if (shade)
        for (i = 0; i < count; i++)
            pixels[i] = (pixels[i] >> 24) ? pixels[i] : (uint32_t)shade << 24;  /* black, as strong as the picture is dark */
}

static void picture_report(void)
{
    fprintf(stderr, "picture: %lu frames %d wide, hash %08X%c", picture_count, test_render, (unsigned)picture_hash, 10);
}

/* ---- picture tests (SS_BENCH, see the top of this file and tools/tests/flicker_bench.py) ----
 *
 * Without a window only a dumped frame is drawn, but what the renderer does about sprites that alternate depends
 * on the frames before. So here every frame is drawn, as with a window, and a run of them is written out.
 * A script can change what is drawn: test sprites, their tiles and colours, registers, a tile map. It is a text
 * file, one command a line:
 *     F <frame>                 the commands that follow are for this frame only (before the first F: every frame)
 *     m <address> <value>       16 bits (hex) of video memory: registers (on every line), palette, VRAM, sprite table
 *     v <dx> <dy>               move the camera of a field scene by this many pixels (decimal)
 *     g <address> <bits>        set these bits (hex, 16 of them) in the game's own memory, and leave them set
 * All but `g` are applied just before the frame is drawn and taken back right after, so the game never sees them
 * and plays exactly as without the test. `g` is for making the game itself alternate a sprite (a flag of the
 * player's record, say): the game then plays on with it.
 * Each frame written: "SSFR", frame, width, whether it is a field scene, the camera's x and y (32-bit each), the
 * sprite table as drawn (1 KiB), the renderer's picture, the picture as it would be shown (both width x 160 x 4). */
#ifdef SS_PICTURE_BENCH              /* the development build only (tools/link.py): it needs the full C library */
static void bench_frame(void)
{
    struct command { unsigned long frame; char kind; int a, b; };
    static const struct { uint32_t base, size; } parts[] = {
        { SS_IO, SS_LINE_REGISTERS }, { SS_PALETTE, 0x400 }, { SS_VRAM, 0x18000 }, { SS_OAM, 0x400 },
    };
    static struct command *commands;
    static int command_count, state;
    static unsigned long first, count, drawn;
    static double seconds;
    static FILE *out;
    static uint32_t shot[SS_MAX_VIEW_W * SS_SCREEN_H];
    static uint8_t kept[SS_LINE_REGISTERS + 0x400 + 0x18000 + 0x400], kept_lines[SS_SCREEN_H][SS_LINE_REGISTERS];
    struct ss_view view;
    int width = forced_width ? forced_width : SS_SCREEN_W, i, y, n, dx = 0, dy = 0;
    uint8_t *save = kept;
    uint32_t t0;

    if (!state) {
        const char *text = getenv("SS_BENCH"), *path = getenv("SS_BENCH_SCRIPT");
        state = -1;
        if (!text)
            return;
        state = 1;
        first = strtoul(text, (char **)&text, 10);
        count = *text == ':' ? strtoul(text + 1, NULL, 10) : 0;
        if (count)
            out = fopen(getenv("SS_BENCH_OUT") ? getenv("SS_BENCH_OUT") : "ss_bench.bin", "wb");
        if (path) {
            FILE *file = fopen(path, "rb");
            char line[80];
            unsigned long frame = 0;
            commands = (struct command *)calloc(0x40000, sizeof *commands);
            while (file && commands && command_count < 0x40000 && fgets(line, sizeof line, file)) {
                struct command *c = &commands[command_count];
                c->frame = frame;
                c->kind = line[0];
                if (line[0] == 'F') frame = strtoul(line + 1, NULL, 10);
                else if ((line[0] == 'm' || line[0] == 'g') && sscanf(line + 1, "%x %x", (unsigned *)&c->a, (unsigned *)&c->b) == 2) command_count++;
                else if (line[0] == 'v' && sscanf(line + 1, "%d %d", &c->a, &c->b) == 2) command_count++;
            }
            if (file) fclose(file);
        }
    }
    if (state < 0 || frame_count + 40 < first)
        return;
    for (n = 0; n < 4; n++) {
        memcpy(save, (const void *)(uintptr_t)parts[n].base, parts[n].size);
        save += parts[n].size;
    }
    memcpy(kept_lines, line_registers, sizeof kept_lines);
    for (i = 0; i < command_count; i++) {
        const struct command *c = &commands[i];
        uint32_t address = (uint32_t)c->a;
        if (c->frame && c->frame != frame_count)
            continue;
        if (c->kind == 'v') {
            dx += c->a;
            dy += c->b;
            continue;
        }
        if (c->kind == 'g') {
            if ((address >= SS_EWRAM && address < SS_EWRAM + 0x3FFFF) || (address >= SS_IWRAM && address < SS_IWRAM + 0x7FFF))
                *(uint16_t *)(uintptr_t)address |= (uint16_t)c->b;
            continue;
        }
        for (n = 0; n < 4; n++)
            if (address >= parts[n].base && address + 2 <= parts[n].base + parts[n].size)
                memcpy((void *)(uintptr_t)address, &c->b, 2);
        if (address - SS_IO <= SS_LINE_REGISTERS - 2)
            for (y = 0; y < SS_SCREEN_H; y++)
                memcpy(&line_registers[y][address - SS_IO], &c->b, 2);
    }
    if (!ss_field_view(&view, width)) {
        memset(&view, 0, sizeof view);
        view.width = width;
    }
    for (n = 0; n < 4 && (dx || dy); n++)
        if (view.bg[n].map) {           /* the camera moved: the layer's place in its map and its scroll registers */
            view.bg[n].scroll_x += dx;
            view.bg[n].scroll_y += dy;
            REG16(0x10 + n * 4) = (uint16_t)(REG16(0x10 + n * 4) + dx);
            REG16(0x12 + n * 4) = (uint16_t)(REG16(0x12 + n * 4) + dy);
            for (y = 0; y < SS_SCREEN_H; y++)
                memcpy(&line_registers[y][0x10 + n * 4], (const void *)(uintptr_t)(SS_IO + 0x10 + n * 4), 4);
        }
    t0 = ss_plat_ticks_ms();
    ss_ppu_render(shot, width, &view);
    seconds += (ss_plat_ticks_ms() - t0) / 1000.0;
    drawn++;
    if (out && frame_count >= first && frame_count < first + count) {
        uint32_t head[6] = { 0x52465353u, (uint32_t)frame_count, (uint32_t)width, (uint32_t)view.world, 0, 0 };
        for (n = 3; n >= 0; n--)
            if (view.world && view.bg[n].map) {
                head[4] = (uint32_t)view.bg[n].scroll_x;
                head[5] = (uint32_t)view.bg[n].scroll_y;
            }
        fwrite(head, 4, 6, out);
        fwrite((const void *)(uintptr_t)SS_OAM, 1, 0x400, out);
        fwrite(shot, 4, (size_t)width * SS_SCREEN_H, out);
        finish_picture(shot, width);
        fwrite(shot, 4, (size_t)width * SS_SCREEN_H, out);
    } else {
        finish_picture(shot, width);
    }
    for (n = 0, save = kept; n < 4; n++) {
        memcpy((void *)(uintptr_t)parts[n].base, save, parts[n].size);
        save += parts[n].size;
    }
    memcpy(line_registers, kept_lines, sizeof kept_lines);
    if (out && frame_count + 1 == first + count) {
        fclose(out);
        out = NULL;
    }
    if (frame_count == frame_limit)
        fprintf(stderr, "bench: %lu frames drawn, %.1f microseconds a frame in the renderer%c", drawn, seconds * 1e6 / (double)drawn, 10);
}
#else
static void bench_frame(void) { }
#endif

void VBlankIntrWait(void)
{
    if (frame_count)
        ss_prof(SS_PROF_LOGIC, 0);
    prof_frame_end(frame_count);
    ss_prof(SS_PROF_NET, 1);
    ss_net_flush();             /* online: what this frame wrote goes out before the frame's time is waited out */
    ss_prof(SS_PROF_NET, 0);
    if (frame_count == 0) {
        const char *text = getenv("SS_FRAMES");
        frame_limit = text ? strtoul(text, NULL, 10) : 0;
        text = getenv("SS_DUMP");
        dump_at = text ? strtoul(text, NULL, 10) : 0;
        input_script = getenv("SS_INPUT");
        text = getenv("SS_INPUT_FILE");         /* the same script in a file: long ones do not fit an environment */
        if (text) {
            static char script[0x40000];
            FILE *file = fopen(text, "rb");
            if (file) {
                script[fread(script, 1, sizeof script - 1, file)] = 0;
                fclose(file);
                input_script = script;
            }
        }
        text = getenv("SS_VIEW");             /* SS_VIEW=284 forces the picture width; SS_VIEW=240 turns wide view off */
        forced_width = text ? (int)strtoul(text, NULL, 10) : 0;
        text = getenv("SS_MODE");             /* classic: the console's picture, whatever the window */
        if (text && !memcmp(text, "classic", 8)) forced_width = SS_SCREEN_W;
        if (forced_width && forced_width < SS_SCREEN_W) forced_width = SS_SCREEN_W;
        if (forced_width > SS_MAX_VIEW_W) forced_width = SS_MAX_VIEW_W;
        REG_KEYINPUT = 0x03FF;          /* no button held */
        windowed = !frame_limit;
        text = getenv("SS_TEST_RENDER");
        test_render = text && frame_limit ? (int)strtoul(text, NULL, 10) : 0;
        if (test_render) {
            test_render = test_render < SS_SCREEN_W ? SS_SCREEN_W : test_render > SS_MAX_VIEW_W ? SS_MAX_VIEW_W : test_render;
            atexit(picture_report);
        }
        if (windowed && ss_plat_video_init("ShiningSource") != 0)
            exit(1);
    }
    ss_prof(SS_PROF_BLANK, 1);
    REG_VCOUNT = 160;
    REG_DISPSTAT |= 1;                  /* in vertical blank */
    if (REG_DISPSTAT & 0x0008)
        ss_raise_interrupt(0x0001);
    walk_lines();
    ss_prof(SS_PROF_BLANK, 0);
    frame_count++;
    enemy_log();
    state_log();
    scene_log();
    ss_prof(SS_PROF_EXTRAS, 1);
    ss_rebel_frame();                   /* cheats of a Rebel run, then the platform's achievements (core/rebel.c) */
    ss_prof(SS_PROF_EXTRAS, 0);
    if (test_render) {                  /* SS_TEST_RENDER: the picture a window would show, drawn and thrown away */
        uint32_t *pixels = frame_pixels;
        struct ss_view view;
        int i, count = test_render * SS_SCREEN_H;
        if (!ss_field_view(&view, test_render)) {
            memset(&view, 0, sizeof view);
            view.width = test_render;
        }
        ss_prof(SS_PROF_RENDER, 1);
        ss_ppu_render(pixels, test_render, &view);
        ss_prof(SS_PROF_RENDER, 0);
        ss_prof(SS_PROF_FINISH, 1);
        finish_picture(pixels, test_render);
        ss_prof(SS_PROF_FINISH, 0);
        for (i = 0; i < count; i++)
            picture_hash = (picture_hash ^ pixels[i]) * 16777619u;
        picture_count++;
    }
    if (dump_at && frame_count == dump_at)
        dump_video();
    if (frame_limit)
        bench_frame();
    if (frame_limit) {
        static int every = -1;
        if (every < 0)
            every = getenv("SS_HASH_ALL") != NULL;     /* one line per frame, to find where two builds diverge */
        if (every || frame_count <= 5 || frame_count % 60 == 0 || frame_count == frame_limit)
            fprintf(stderr, "frame %5lu  hash %08X  stubs reached %d\n", frame_count, (unsigned)ss_frame_hash(), ss_stub_count());
        if (frame_count >= frame_limit) {
            ss_audio_report();
            exit(0);
        }
        {                               /* SS_PACE=1: real time without a window, for network tests */
            static int paced = -1;
            static uint32_t started;
            static unsigned long fast_until, fast_by = 1;
            unsigned long due;
            if (paced < 0) {
                /* SS_PACE_FAST=F:K: the first F frames at K times the speed. A session takes five minutes to
                 * get from the title into the field; tests of what happens there do not have to sit them out. */
                const char *text = getenv("SS_PACE_FAST");
                char *end = NULL;
                paced = getenv("SS_PACE") != NULL;
                fast_until = text ? strtoul(text, &end, 10) : 0;
                if (end && *end == ':' && strtoul(end + 1, NULL, 10) > 1)
                    fast_by = strtoul(end + 1, NULL, 10);
                started = ss_plat_ticks_ms() - (uint32_t)(frame_count * 1000 / 60);
            }
            due = frame_count <= fast_until ? frame_count * 1000 / (60 * fast_by)
                                            : fast_until * 1000 / (60 * fast_by) + (frame_count - fast_until) * 1000 / 60;
            /* With SS_PACE_FAST time that was lost is not made up for (a test session waits at its start until
             * every player is seated): the frames owed would all run in a few milliseconds, hundreds of blocks
             * in a burst no real player produces. */
            if (paced && fast_until && ss_plat_ticks_ms() - started > due + 100)
                started = ss_plat_ticks_ms() - (uint32_t)due - 100;
            while (paced && ss_plat_ticks_ms() - started < due)
                ss_plat_net_wait(1);
        }
    }
    if (windowed) {
        static long quit_at = -1;
        if (quit_at < 0)
            quit_at = getenv("SS_QUIT") ? (long)strtoul(getenv("SS_QUIT"), NULL, 10) : 0;
        if (quit_at && frame_count >= (unsigned long)quit_at)
            exit(0);
    }
    if (windowed) {
        uint32_t *pixels = frame_pixels;
        struct ss_view view;
        int width = forced_width ? forced_width : ss_plat_view_width();
        if (!ss_field_view(&view, width)) {     /* menus and cut-scenes keep the console's composition, centred */
            memset(&view, 0, sizeof view);
            view.width = width;
        }
        {
            static uint16_t held;
            static struct ss_skip_view skip = { 0, NULL, 1, 0 };
            static int late, left_out;
            if (skip.present && late && left_out < 3) {
                /* The machine is more than a frame behind: this frame goes by without its picture, so that the
                 * game keeps its speed. At most three in a row: a quarter of the pictures is always shown. */
                left_out++;
                ss_ppu_unseen(&view);
                ss_prof(SS_PROF_PRESENT, 1);
                held = ss_plat_present(pixels, 0);
                ss_prof(SS_PROF_PRESENT, 0);
                late = held & SS_PRESENT_LATE;
                held &= (uint16_t)~SS_PRESENT_LATE;
            } else if (skip.present) {
                left_out = 0;
                ss_prof(SS_PROF_RENDER, 1);
                ss_ppu_render(pixels, width, &view);
                ss_prof(SS_PROF_RENDER, 0);
                ss_prof(SS_PROF_FINISH, 1);
                finish_picture(pixels, width);
                ss_prof(SS_PROF_FINISH, 0);
                ss_plat_overlay(skip.darkness, skip.text);
                ss_prof(SS_PROF_PRESENT, 1);
                held = ss_plat_present(pixels, width);
                ss_prof(SS_PROF_PRESENT, 0);
                late = held & SS_PRESENT_LATE;
                held &= (uint16_t)~SS_PRESENT_LATE;
            } else if (ss_net_active()) {
                /* A scene being skipped online. Alone the game runs as fast as the machine allows; with other
                 * players it keeps its normal speed: the game counts its waits for them in frames, and run fast
                 * every one of them runs out at once (it then reports a communication error). Online the scene
                 * is ended by the game itself instead (core/scene.c). */
                static uint32_t due;
                uint32_t now = ss_plat_ticks_ms();
                if (now - due > 200u)
                    due = now;
                while ((int32_t)(ss_plat_ticks_ms() - due) < 0)
                    ss_plat_net_wait(1);
                due += 16 + (due % 3 == 0);     /* 60 frames a second */
            }
            if (input_script) {                 /* tests: the script plays the part of the player */
                scripted_input();
                /* the script holds the ten buttons; the extra ones may also come from the platform */
                held = (uint16_t)((~REG_KEYINPUT & 0x03FF) | script_extra | (held & (SS_KEY_L_BACK | SS_KEY_R_BACK)));
            }
            /* the platform's extra buttons become L and R here (core/qol.c); the register is active low */
            REG_KEYINPUT = (uint16_t)(~ss_skip_update(ss_qol_keys(held), &skip) & 0x03FF);
            skip_mute = skip.mute;
        }
    } else {
        static struct ss_skip_view skip;        /* without a window the skip still plays, so tests can drive it */
        if (input_script)
            scripted_input();
        REG_KEYINPUT = (uint16_t)(~ss_skip_update(ss_qol_keys((uint16_t)((~REG_KEYINPUT & 0x03FF) | script_extra)), &skip) & 0x03FF);
        skip_mute = skip.mute;
        skip_fast = skip.mute;
    }
    REG_DISPSTAT &= ~1;
    REG_VCOUNT = 0;
    {                                   /* SS_TEST_SHOP=N, SS_TEST_SHOP_KIND=K: at frame N start the game's item collection menu K (1 shop) as an event task */
        static long shop_at = -1;
        static uint32_t record[10];
        void sub_08031B54(void); void sub_0803188C(void); void sub_080319F0(void); void sub_0805782C(uint32_t);
        uint32_t sub_08000274(uint32_t, uint32_t, uint32_t);
        if (shop_at < 0)
            shop_at = getenv("SS_TEST_SHOP") ? (long)strtoul(getenv("SS_TEST_SHOP"), NULL, 10) : 0;
        if (shop_at && frame_count == (unsigned long)shop_at && getenv("SS_TEST_SHOP_ONLY")) {   /* online tests: only the player whose folder holds this file */
            FILE *mark = fopen(getenv("SS_TEST_SHOP_ONLY"), "rb");
            if (mark) fclose(mark); else shop_at = 0;
        }
        if (shop_at && frame_count == (unsigned long)shop_at) {
            int kind = getenv("SS_TEST_SHOP_KIND") ? (int)strtoul(getenv("SS_TEST_SHOP_KIND"), NULL, 10) : 1;
            memset(record, 0, sizeof record);
            record[2] = 0x50;
            record[3] = kind == 1 ? (uint32_t)(uintptr_t)sub_08031B54 : kind == 5 ? (uint32_t)(uintptr_t)sub_0803188C
                        : kind == 6 ? (uint32_t)(uintptr_t)sub_080319F0 : (uint32_t)(uintptr_t)sub_0805782C;
            fprintf(stderr, "shop test: kind %d started, result %u%c", kind, (unsigned)sub_08000274((uint32_t)(uintptr_t)record, (uint32_t)kind, 0), 10);
        }
    }
    test_warp();
    where_log();
    {                                   /* (after the blanking flag is cleared: the game waits for that before it writes) SS_TEST_SAVE=N: at frame N make the game save the current character, report its answer */
        static long save_at = -1;
        unsigned int sub_0807A124(void);        /* SaveCurrentCharacterSlot: 0 means written and verified */
        if (save_at < 0)
            save_at = getenv("SS_TEST_SAVE") ? (long)strtoul(getenv("SS_TEST_SAVE"), NULL, 10) : 0;
        if (save_at && frame_count == (unsigned long)save_at)
            fprintf(stderr, "save test: the game's own save routine returned %u (0 = saved and verified)%c", sub_0807A124(), 10);
    }
    ss_prof(SS_PROF_LOGIC, 1);
}
