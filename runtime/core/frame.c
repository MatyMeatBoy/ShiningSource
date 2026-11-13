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

static void scripted_input(void)
{
    const char *cursor = input_script;
    uint16_t held = 0;
    while (cursor && *cursor) {
        char *end;
        unsigned long from = strtoul(cursor, &end, 10), mask;
        if (*end != ':')
            break;
        mask = strtoul(end + 1, &end, 16);
        if (from <= frame_count)
            held = (uint16_t)mask;
        cursor = *end == ',' ? end + 1 : NULL;
    }
    REG_KEYINPUT = (uint16_t)(~held & 0x03FF);
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

void VBlankIntrWait(void)
{
    ss_net_flush();             /* online: what this frame wrote goes out before the frame's time is waited out */
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
        if (windowed && ss_plat_video_init("ShiningSource") != 0)
            exit(1);
    }
    REG_VCOUNT = 160;
    REG_DISPSTAT |= 1;                  /* in vertical blank */
    if (REG_DISPSTAT & 0x0008)
        ss_raise_interrupt(0x0001);
    walk_lines();
    frame_count++;
    ss_rebel_frame();                   /* cheats of a Rebel run, then the platform's achievements (core/rebel.c) */
    if (dump_at && frame_count == dump_at)
        dump_video();
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
            if (paced < 0) {
                paced = getenv("SS_PACE") != NULL;
                started = ss_plat_ticks_ms() - (uint32_t)(frame_count * 1000 / 60);
            }
            while (paced && ss_plat_ticks_ms() - started < frame_count * 1000 / 60)
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
        static uint32_t pixels[SS_MAX_VIEW_W * SS_SCREEN_H];
        struct ss_view view;
        int width = forced_width ? forced_width : ss_plat_view_width();
        if (!ss_field_view(&view, width)) {     /* menus and cut-scenes keep the console's composition, centred */
            memset(&view, 0, sizeof view);
            view.width = width;
        }
        {
            static uint16_t held;
            static struct ss_skip_view skip = { 0, NULL, 1, 0 };
            if (skip.present) {
                ss_ppu_render(pixels, width, &view);
                ss_plat_overlay(skip.darkness, skip.text);
                held = ss_plat_present(pixels, width);
            } else if (ss_net_active()) {
                /* A scene being skipped online. Alone the game runs as fast as the machine allows; with other
                 * players it may not: the game counts its waits for them in frames, and at a thousand frames a
                 * second every one of them runs out at once (it then reports a communication error). Twice
                 * the normal speed is what those waits can take. */
                static uint32_t due;
                uint32_t now = ss_plat_ticks_ms();
                if (now - due > 200u)
                    due = now;
                while ((int32_t)(ss_plat_ticks_ms() - due) < 0)
                    ss_plat_net_wait(1);
                due += 8;
            }
            if (input_script) {                 /* tests: the script plays the part of the player */
                scripted_input();
                held = (uint16_t)(~REG_KEYINPUT & 0x03FF);
            }
            REG_KEYINPUT = (uint16_t)(~ss_skip_update(held, &skip) & 0x03FF);   /* the register is active low */
            skip_mute = skip.mute;
        }
    } else {
        static struct ss_skip_view skip;        /* without a window the skip still plays, so tests can drive it */
        if (input_script)
            scripted_input();
        REG_KEYINPUT = (uint16_t)(~ss_skip_update((uint16_t)(~REG_KEYINPUT & 0x03FF), &skip) & 0x03FF);
        skip_mute = skip.mute;
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
    {                                   /* (after the blanking flag is cleared: the game waits for that before it writes) SS_TEST_SAVE=N: at frame N make the game save the current character, report its answer */
        static long save_at = -1;
        unsigned int sub_0807A124(void);        /* SaveCurrentCharacterSlot: 0 means written and verified */
        if (save_at < 0)
            save_at = getenv("SS_TEST_SAVE") ? (long)strtoul(getenv("SS_TEST_SAVE"), NULL, 10) : 0;
        if (save_at && frame_count == (unsigned long)save_at)
            fprintf(stderr, "save test: the game's own save routine returned %u (0 = saved and verified)%c", sub_0807A124(), 10);
    }
}
