/* Platform services through SDL3: window, presentation, keyboard, controller and touch screen, sound output,
 * frame pacing. Used natively by the dev32 build and by the 64-bit hosts (Windows, Linux, Android); the game side
 * only sees the ss_plat_* calls. */
#include "shiningsource/runtime.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_Gamepad *gamepad;
static SDL_AudioStream *audio;
static uint64_t next_frame_ns;
/* Who keeps time (SS_VSYNC): 1, the default, synchronises with the display; when its refresh rate is a whole
 * multiple of about 60 Hz the display itself paces the game (no tearing, no stutter; the game then runs 0.45%
 * fast and the sound is resampled by the same amount). 2 is for variable-refresh displays: the game keeps its own
 * 59.7275 Hz and the display follows. 0 turns synchronisation off. Online, every player keeps the console's own
 * rate so that nobody has to wait for anybody: the display only removes tearing. */
static int display_paced;

/* ---- measurements (SS_STATS=1): how regular the frames were and how much memory the process used ----
 * Printed when the program ends. Intervals are the real time between two presented frames; the console's is
 * 16.74 ms. Late frames are those more than 2 ms over it. */
enum { STAT_SLOTS = 1 << 16 };
static uint32_t *stat_interval_us;
static unsigned stat_count;
static uint64_t stat_last_ns, stat_waits, stat_wait_ns;

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static void memory_report(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters))
        fprintf(stderr, "stats: memory in use %.1f MiB (peak %.1f), committed %.1f MiB (peak %.1f)\n",
                counters.WorkingSetSize / 1048576.0, counters.PeakWorkingSetSize / 1048576.0,
                counters.PrivateUsage / 1048576.0, counters.PeakPagefileUsage / 1048576.0);
}
#else
static void memory_report(void)
{
    FILE *file = fopen("/proc/self/status", "r");
    char line[160];
    while (file && fgets(line, sizeof line, file))
        if (!strncmp(line, "VmRSS", 5) || !strncmp(line, "VmHWM", 5) || !strncmp(line, "VmSize", 6))
            fprintf(stderr, "stats: %s", line);
    if (file)
        fclose(file);
}
#endif

static int by_value(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void stats_report(void)
{
    unsigned n = stat_count < STAT_SLOTS ? stat_count : STAT_SLOTS, i, late = 0;
    uint64_t sum = 0;
    if (n > 60) {
        for (i = 0; i < n; i++) {
            sum += stat_interval_us[i];
            late += stat_interval_us[i] > 16743 + 2000;
        }
        qsort(stat_interval_us, n, sizeof *stat_interval_us, by_value);
        fprintf(stderr, "stats: %u frames, interval mean %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f; %u late (%.2f%%)\n",
                n, sum / 1000.0 / n, stat_interval_us[n / 2] / 1000.0, stat_interval_us[n * 95 / 100] / 1000.0,
                stat_interval_us[n * 99 / 100] / 1000.0, stat_interval_us[n - 1] / 1000.0, late, late * 100.0 / n);
        fprintf(stderr, "stats: waited for the next frame %.1f%% of the time (the rest is the game, the picture and the system)\n",
                stat_wait_ns * 100.0 / (sum * 1000.0));
    }
    memory_report();
}

/* GBA buttons, bit order of KEYINPUT: A B Select Start Right Left Up Down R L */
static SDL_Scancode keys[10] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V, SDL_SCANCODE_C,
};
/* controller buttons in the same order; the east button is A as on the console's layout */
static SDL_GamepadButton pad_buttons[10] = {
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
};

/* SS_KEYS and SS_PAD: ten numbers separated by commas (SDL scancodes, SDL controller buttons) in the order
 * above. The launcher writes them; a number out of range or a short list keeps the default of that entry. */
static void read_bindings(const char *name, int *out, int lowest, int limit)
{
    const char *text = getenv(name);
    int i;
    for (i = 0; text && *text && i < 10; i++) {
        char *end;
        long value = strtol(text, &end, 10);
        if (end == text)
            break;
        if (value >= lowest && value < limit)    /* controller buttons start at 0 (the bottom face button) */
            out[i] = (int)value;
        text = *end == ',' ? end + 1 : end;
    }
}

int ss_plat_video_init(const char *title)
{
    SDL_AudioSpec spec;

#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");     /* otherwise a resizable window follows the user */
#endif
    read_bindings("SS_KEYS", (int *)keys, 1, SDL_SCANCODE_COUNT);
    read_bindings("SS_PAD", (int *)pad_buttons, 0, SDL_GAMEPAD_BUTTON_COUNT);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 48000;
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        const char *volume = getenv("SS_VOLUME");      /* 0..100, set by the launcher */
        if (volume)
            SDL_SetAudioStreamGain(audio, (float)atoi(volume) / 100.0f);
        SDL_ResumeAudioStreamDevice(audio);
    }
    else
        fprintf(stderr, "no audio device: %s\n", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer(title, SS_SCREEN_W * 4, SS_SCREEN_H * 4, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    {
        const char *text = getenv("SS_VSYNC");
        int wanted = text ? atoi(text) : 1;
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        float refresh = mode && mode->refresh_rate > 1.0f ? mode->refresh_rate : 60.0f;
        int every = (int)(refresh / 60.0f + 0.5f);
        if (wanted == 2) {
            if (!SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE))
                SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
        } else if (wanted == 1) {
            float rate = every > 0 ? refresh / (float)every : 0.0f;
            if (every >= 1 && rate > 58.5f && rate < 61.5f && !getenv("SS_NET") && SDL_SetRenderVSync(renderer, every)) {
                display_paced = 1;
                if (audio)
                    SDL_SetAudioStreamFrequencyRatio(audio, rate / 59.7275f);
            } else {
                SDL_SetRenderVSync(renderer, 1);
            }
        } else {
            SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
        }
        fprintf(stderr, "video: display %.2f Hz, %s\n", refresh,
                display_paced ? "paced by the display" : wanted == 2 ? "variable refresh, own pacing" : wanted == 1 ? "vsync, own pacing" : "no vsync");
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SS_MAX_VIEW_W, SS_SCREEN_H);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);      /* transparent pixels let the side field show */
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(window, true);
#else
    if (getenv("SS_FULLSCREEN"))
        SDL_SetWindowFullscreen(window, true);
#endif
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);                /* come to the front even when started from a console or a dialog */
    next_frame_ns = SDL_GetTicksNS();
    if (getenv("SS_STATS")) {
        stat_interval_us = calloc(STAT_SLOTS, sizeof *stat_interval_us);
        atexit(stats_report);
    }
    return 0;
}

void ss_plat_audio(const int16_t *samples, int frames)
{
    /* never let more than about a tenth of a second queue up: late sound is worse than a dropped frame */
    if (audio && SDL_GetAudioStreamQueued(audio) < 48000 * 4 / 10)
        SDL_PutAudioStreamData(audio, samples, frames * 4);
}

/* The picture keeps a whole-number scale (the largest that fits the height of the window) and takes as many
 * columns as the width then allows, so a 16:9 window shows 284 columns and an ultrawide one up to 480. */
/* ---- screen modes ----
 * Enhanced (the default): the picture takes the width the window allows, see ss_plat_view_width.
 * Classic (SS_MODE=classic): the console's 240 x 160 picture at the largest whole-number scale that fits, inside
 * a border. SS_BORDER is its thickness in console pixels (0 to 64) and SS_BORDER_STYLE its look (0 black,
 * 1 graphite, 2 indigo, 3 silver, 4 "Soul", the default). The border is drawn here, with no artwork of any game
 * or console. */
static int classic = -1, border, border_style, sides_field = 1;

static void read_mode(void)
{
    const char *text = getenv("SS_MODE");
    classic = text && !strcmp(text, "classic");
    border = getenv("SS_BORDER") ? atoi(getenv("SS_BORDER")) : 16;
    if (border < 0) border = 0;
    if (border > 64) border = 64;
    sides_field = !(getenv("SS_SIDES") && getenv("SS_SIDES")[0] == '0');
    border_style = getenv("SS_BORDER_STYLE") ? atoi(getenv("SS_BORDER_STYLE")) : 4;
    if (border_style < 0 || border_style > 4) border_style = 4;
}

static void fill(float x, float y, float w, float h, int r, int g, int b)
{
    SDL_FRect box = { x, y, w, h };
    SDL_SetRenderDrawColor(renderer, (Uint8)r, (Uint8)g, (Uint8)b, 255);
    SDL_RenderFillRect(renderer, &box);
}

/* A small jewel: a diamond of `size` rows with a light facet. */
static void jewel(int cx, int cy, int size, int r, int g, int b)
{
    int row;
    for (row = -size; row <= size; row++) {
        int half = size - (row < 0 ? -row : row);
        fill((float)(cx - half), (float)(cy + row), (float)(2 * half + 1), 1, r, g, b);
    }
    fill((float)(cx - size / 2), (float)(cy - size / 2), (float)(size / 2 + 1), 1, 255, 250, 220);
}

/* The field of the Soul look: blue-green, darker towards the bottom, with a fixed pattern of points of light.
 * Nothing is drawn inside the given box (where the picture goes). */
static void draw_field(int w, int h, int box_left, int box_top, int box_right, int box_bottom)
{
    int x, y, k;
    for (y = 0; y < h; y++) {
        int t = y * 255 / (h > 1 ? h - 1 : 1);
        fill(0, (float)y, (float)w, 1, 20 - t * 10 / 255, 62 - t * 34 / 255, 74 - t * 36 / 255);
    }
    for (y = 3; y < h; y += 7)
        for (x = 3 + (y * 5) % 11; x < w; x += 13) {
            if (x >= box_left && x < box_right && y >= box_top && y < box_bottom)
                continue;
            k = (x * 31 + y * 17) % 5;
            fill((float)x, (float)y, 1, 1, 60 + k * 22, 120 + k * 20, 130 + k * 20);
        }
}

/* Style 4, "Soul": the port's own frame in the spirit of the game's menus. A deep blue-green field that darkens
 * towards the bottom, faint lattice of light points like a night sky, a gold frame with a bright and a dark
 * line, and jewels at the corners and the middles of the sides. Everything is computed; nothing is copied from
 * the cartridge. All measures follow the border's thickness, so it holds from 4 pixels up. */
static void draw_soul_border(void)
{
    const int w = SS_SCREEN_W + 2 * border, h = SS_SCREEN_H + 2 * border;
    const int left = border, top = border, right = border + SS_SCREEN_W, bottom = border + SS_SCREEN_H;
    int k, size = border >= 20 ? 4 : border >= 12 ? 3 : 2;

    draw_field(w, h, left - 3, top - 3, right + 3, bottom + 3);
    if (border < 4) {
        fill((float)left - 1, (float)top - 1, SS_SCREEN_W + 2, SS_SCREEN_H + 2, 232, 168, 56);
        return;
    }
    /* outer rim of the whole border */
    fill(0, 0, (float)w, 1, 232, 168, 56); fill(0, (float)h - 1, (float)w, 1, 150, 96, 24);
    fill(0, 0, 1, (float)h, 232, 168, 56); fill((float)w - 1, 0, 1, (float)h, 150, 96, 24);
    /* the frame around the picture: dark seat, gold, bright line, gold */
    fill((float)left - 4, (float)top - 4, SS_SCREEN_W + 8, SS_SCREEN_H + 8, 96, 58, 16);
    fill((float)left - 3, (float)top - 3, SS_SCREEN_W + 6, SS_SCREEN_H + 6, 232, 168, 56);
    fill((float)left - 2, (float)top - 2, SS_SCREEN_W + 4, SS_SCREEN_H + 4, 255, 226, 138);
    fill((float)left - 1, (float)top - 1, SS_SCREEN_W + 2, SS_SCREEN_H + 2, 40, 24, 8);
    if (border >= 8) {
        const int reach = border / 2 > 5 ? border / 2 : 5;          /* how far the jewels sit from the picture */
        const int px[3] = { left - 3, (left + right) / 2, right + 2 }, py[3] = { top - 3, (top + bottom) / 2, bottom + 2 };
        (void)reach;
        for (k = 0; k < 3; k++) {
            jewel(px[k], top - 3, size, k == 1 ? 92 : 232, k == 1 ? 214 : 168, k == 1 ? 226 : 56);
            jewel(px[k], bottom + 2, size, k == 1 ? 92 : 232, k == 1 ? 214 : 168, k == 1 ? 226 : 56);
        }
        jewel(left - 3, py[1], size, 92, 214, 226);
        jewel(right + 2, py[1], size, 92, 214, 226);
    }
    if (border >= 14) {                               /* a second, thin gold line half way out */
        const int out = border / 2;
        fill((float)left - out, (float)top - out, (float)(SS_SCREEN_W + 2 * out), 1, 170, 118, 40);
        fill((float)left - out, (float)bottom + out - 1, (float)(SS_SCREEN_W + 2 * out), 1, 170, 118, 40);
        fill((float)left - out, (float)top - out, 1, (float)(SS_SCREEN_H + 2 * out), 170, 118, 40);
        fill((float)right + out - 1, (float)top - out, 1, (float)(SS_SCREEN_H + 2 * out), 170, 118, 40);
    }
}

static void draw_border(void)
{
    static const uint8_t body[4][3] = { { 0, 0, 0 }, { 44, 46, 52 }, { 62, 52, 128 }, { 176, 180, 190 } };
    const uint8_t *c = body[border_style & 3];
    SDL_FRect box;
    int i;
    if (border_style == 4) {
        SDL_SetRenderDrawColor(renderer, 10, 28, 38, 255);      /* what shows outside the whole-number scale */
        SDL_RenderClear(renderer);
        draw_soul_border();
        return;
    }
    SDL_SetRenderDrawColor(renderer, c[0], c[1], c[2], 255);
    SDL_RenderClear(renderer);
    if (!border || !border_style)
        return;
    for (i = 0; i < 2 && i < border; i++) {         /* a lighter edge outside, a dark seat around the picture */
        box = (SDL_FRect){ (float)i, (float)i, (float)(SS_SCREEN_W + 2 * border - 2 * i), (float)(SS_SCREEN_H + 2 * border - 2 * i) };
        SDL_SetRenderDrawColor(renderer, (Uint8)(c[0] + (255 - c[0]) / (3 + i)), (Uint8)(c[1] + (255 - c[1]) / (3 + i)),
                               (Uint8)(c[2] + (255 - c[2]) / (3 + i)), 255);
        SDL_RenderRect(renderer, &box);
    }
    box = (SDL_FRect){ (float)border - 2, (float)border - 2, SS_SCREEN_W + 4, SS_SCREEN_H + 4 };
    SDL_SetRenderDrawColor(renderer, (Uint8)(c[0] / 3), (Uint8)(c[1] / 3), (Uint8)(c[2] / 3), 255);
    SDL_RenderFillRect(renderer, &box);
}

int ss_plat_view_width(void)
{
    int w = 0, h = 0, scale, width;
    if (classic < 0)
        read_mode();
    if (classic)
        return SS_SCREEN_W;
    if (!window || !SDL_GetWindowSizeInPixels(window, &w, &h) || h < SS_SCREEN_H)
        return SS_SCREEN_W;
    scale = h / SS_SCREEN_H;
    width = (w / scale) & ~1;
    if (width < SS_SCREEN_W) width = SS_SCREEN_W;
    if (width > SS_MAX_VIEW_W) width = SS_MAX_VIEW_W;
    return width;
}

static void handle_event(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT || (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE))
        exit(0);
    if (event->type == SDL_EVENT_GAMEPAD_ADDED && !gamepad)
        gamepad = SDL_OpenGamepad(event->gdevice.which);
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED && gamepad && SDL_GetGamepadID(gamepad) == event->gdevice.which) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_F11)
        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}

void ss_plat_status(const char *text)
{
    char title[160];
    if (!window)
        return;
    if (text && *text)
        SDL_snprintf(title, sizeof title, "ShiningSource - %s", text);
    SDL_SetWindowTitle(window, text && *text ? title : "ShiningSource");
}

void ss_plat_pump(void)
{
    SDL_Event event;
    if (!window)
        return;
    while (SDL_PollEvent(&event))
        handle_event(&event);
}

/* ---- overlay: the fade and the one line of text of "skip scene" ---- */
static int overlay_darkness, overlay_skipping;
static char overlay_text[64];

void ss_plat_overlay(int darkness, const char *text)
{
    overlay_darkness = darkness < 0 ? 0 : darkness > 255 ? 255 : darkness;
    overlay_skipping = 0;
    SDL_snprintf(overlay_text, sizeof overlay_text, "%s", text ? text : "");
}

/* 5 x 7 dots per character, one byte per row, for the characters the messages use. */
static const uint8_t *glyph(char c)
{
    static const uint8_t letters[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
        {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F} };
    static const uint8_t digits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C} };
    static const uint8_t question[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, equals[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
                         slash[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, colon[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
                         dot[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, blank[7] = {0};
    /* the rest of what an achievement's title may hold */
    static const uint8_t bang[7] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, quote[7] = {0x04,0x04,0x08,0x00,0x00,0x00,0x00},
                         comma[7] = {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, dash[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
                         paren_l[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, paren_r[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
                         plus[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, percent[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
                         amp[7] = {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D};
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case '?': return question;
    case '=': return equals;
    case '/': return slash;
    case ':': return colon;
    case '.': return dot;
    case '!': return bang;
    case '\'': return quote;
    case ',': return comma;
    case '-': return dash;
    case '(': return paren_l;
    case ')': return paren_r;
    case '+': return plus;
    case '%': return percent;
    case '&': return amp;
    }
    return blank;
}

/* One line of 5 x 7 text in the current draw colour. */
static void draw_text(const char *text, int length, int x, int y)
{
    SDL_FRect box;
    int i, row, column;
    for (i = 0; i < length; i++) {
        const uint8_t *rows = glyph(text[i]);
        for (row = 0; row < 7; row++)
            for (column = 0; column < 5; column++)
                if (rows[row] & (0x10 >> column)) {
                    box = (SDL_FRect){ (float)(x + i * 6 + column), (float)(y + row), 1, 1 };
                    SDL_RenderFillRect(renderer, &box);
                }
    }
}

/* Draw the overlay over a picture `width` x `height` units big (the current logical size). */
static void draw_overlay(int width, int height)
{
    SDL_FRect box;
    int length = (int)SDL_strlen(overlay_text), x, y;
    if (overlay_darkness > 0) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)overlay_darkness);
        box = (SDL_FRect){ 0, 0, (float)width, (float)height };
        SDL_RenderFillRect(renderer, &box);
    }
    if (!length)
        return;
    x = (width - length * 6) / 2;
    y = height - 22;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    box = (SDL_FRect){ (float)x - 4, (float)y - 3, (float)(length * 6 + 7), 13 };
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 255, 232, 150, 255);
    draw_text(overlay_text, length, x, y);
}

/* ---- notices: a heading and one line, over the top of the picture for a few seconds ----
 * Used for achievements (runtime/achievements). They wait in a short queue, so two unlocked in the same frame are
 * both read. Time is counted in frames shown. A line longer than the picture slides to its end while it shows. */
enum { NOTICE_FRAMES = 300, NOTICE_FADE = 20, NOTICE_QUEUE = 16, NOTICE_ICON = 20, NOTICE_ICON_MAX = 128 };
struct notice { char heading[48], text[96]; int rebel, icon_size; uint32_t *icon; };    /* icon: square, ARGB, own copy */
static struct notice notices[NOTICE_QUEUE];
static int notice_count, notice_age;
static SDL_Texture *notice_texture;         /* the icon of the notice being shown */

/* Capitals only (the 5 x 7 set has no accents): the accented letters of Latin-1 lose the accent, and any other
 * character outside ASCII becomes one '?'. The text is UTF-8. */
static void notice_copy(char *out, size_t capacity, const char *text)
{
    /* U+00C0..U+00FF without the bit that tells capitals from small letters */
    static const char plain[33] = "AAAAAA?CEEEEIIII?NOOOOO?OUUUUY??";
    size_t n = 0;
    for (; text && *text && n + 1 < capacity; text++) {
        unsigned char c = (unsigned char)*text, next = (unsigned char)text[1];
        if ((c & 0xC0) == 0x80)
            continue;                               /* the tail of a character already shown */
        if (c == 0xC3 && next >= 0x80 && next < 0xC0)
            out[n++] = plain[next & 0x1F];
        else
            out[n++] = c >= 0x80 ? '?' : c >= 'a' && c <= 'z' ? (char)(c - 32) : c < 32 ? ' ' : (char)c;
    }
    out[n] = 0;
}

void ss_plat_notice(const char *heading, const char *text, int rebel, const uint32_t *icon, int icon_size)
{
    struct notice *notice;
    if (notice_count == NOTICE_QUEUE)
        return;
    notice = &notices[notice_count++];
    notice_copy(notice->heading, sizeof notice->heading, heading);
    notice_copy(notice->text, sizeof notice->text, text);
    notice->rebel = rebel;
    notice->icon = NULL;
    notice->icon_size = 0;
    if (icon && icon_size > 0 && icon_size <= NOTICE_ICON_MAX && (notice->icon = malloc((size_t)icon_size * icon_size * 4)) != NULL) {
        memcpy(notice->icon, icon, (size_t)icon_size * icon_size * 4);
        notice->icon_size = icon_size;
    }
}

/* Draw the notice whose turn it is, centred at the top of the picture: `width` columns from (left, top). */
static void draw_notice(int left, int top, int width)
{
    const struct notice *notice = &notices[0];
    int heading, length, fit, first = 0, shown, columns, x, alpha = 255, icon;
    SDL_FRect box;

    if (!notice_count)
        return;
    if (notice->icon && !notice_texture && !notice_age) {
        notice_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, notice->icon_size, notice->icon_size);
        if (notice_texture) {
            SDL_UpdateTexture(notice_texture, NULL, notice->icon, notice->icon_size * 4);
            SDL_SetTextureBlendMode(notice_texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(notice_texture, SDL_SCALEMODE_LINEAR);
        }
    }
    icon = notice_texture ? NOTICE_ICON + 4 : 0;    /* room at the left of the text */
    fit = (width - 16 - icon) / 6;
    heading = (int)SDL_strlen(notice->heading);
    length = (int)SDL_strlen(notice->text);
    if (notice_age < NOTICE_FADE)
        alpha = 255 * (notice_age + 1) / NOTICE_FADE;
    if (notice_age >= NOTICE_FRAMES - NOTICE_FADE)
        alpha = 255 * (NOTICE_FRAMES - notice_age) / NOTICE_FADE;
    if (heading > fit)
        heading = fit;
    shown = length > fit ? fit : length;
    if (length > fit) {                             /* slide: a second still, then a character every eight frames */
        first = (notice_age - 60) / 8;
        if (first < 0) first = 0;
        if (first > length - fit) first = length - fit;
    }
    columns = shown > heading ? shown : heading;
    x = left + (width - columns * 6 - icon) / 2 + icon;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(alpha * 190 / 255));
    box = (SDL_FRect){ (float)(x - 5 - icon), (float)top + 2, (float)(columns * 6 + 9 + icon), 24 };
    SDL_RenderFillRect(renderer, &box);
    if (notice->rebel)
        SDL_SetRenderDrawColor(renderer, 255, 96, 150, (Uint8)alpha);
    else
        SDL_SetRenderDrawColor(renderer, 232, 168, 56, (Uint8)alpha);
    SDL_RenderRect(renderer, &box);
    draw_text(notice->heading, heading, x + (columns - heading) * 3, top + 5);
    SDL_SetRenderDrawColor(renderer, 255, 244, 214, (Uint8)alpha);
    draw_text(notice->text + first, shown, x + (columns - shown) * 3, top + 15);
    if (notice_texture) {
        box = (SDL_FRect){ (float)(x - 2 - icon), (float)top + 4, NOTICE_ICON, NOTICE_ICON };
        SDL_SetTextureAlphaMod(notice_texture, (Uint8)alpha);
        SDL_RenderTexture(renderer, notice_texture, NULL, &box);
    }
    if (++notice_age >= NOTICE_FRAMES) {
        if (notice_texture)
            SDL_DestroyTexture(notice_texture);
        notice_texture = NULL;
        free(notices[0].icon);
        notice_age = 0;
        notice_count--;
        memmove(&notices[0], &notices[1], (size_t)notice_count * sizeof notices[0]);
    }
}

/* ---- touch screen: the console's buttons drawn over the picture ----
 * Positions are in picture units (the view is `width` x 160). On by default on Android while no controller is
 * connected; SS_TOUCH=1 or 0 forces it. */
struct touch_button { int bit; float x, y, w, h; };

static int touch_layout(struct touch_button *b, int width)
{
    float w = (float)width;
    int n = 0;
    b[n++] = (struct touch_button){ 0, w - 36, 90, 30, 30 };                /* A */
    b[n++] = (struct touch_button){ 1, w - 70, 116, 30, 30 };               /* B */
    b[n++] = (struct touch_button){ 2, w / 2 - 36, 146, 30, 12 };           /* Select */
    b[n++] = (struct touch_button){ 3, w / 2 + 6, 146, 30, 12 };            /* Start */
    b[n++] = (struct touch_button){ 9, 2, 2, 44, 20 };                      /* L */
    b[n++] = (struct touch_button){ 8, w - 46, 2, 44, 20 };                 /* R */
    return n;
}

static uint16_t touch_buttons(int width)
{
    static int wanted = -1;
    const float pad_x = 40, pad_y = 116, pad_radius = 36, dead = 9;
    struct touch_button buttons[8];
    SDL_TouchID *devices;
    SDL_FRect box;
    uint16_t held = 0;
    int count = 0, d, i, n, window_w = 1, window_h = 1;

    if (wanted < 0) {
        const char *text = getenv("SS_TOUCH");
#ifdef __ANDROID__
        wanted = text ? atoi(text) : 1;
#else
        wanted = text ? atoi(text) : 0;
#endif
    }
    if (!wanted || gamepad)
        return 0;
    n = touch_layout(buttons, width);
    SDL_GetWindowSize(window, &window_w, &window_h);
    devices = SDL_GetTouchDevices(&count);
    for (d = 0; devices && d < count; d++) {
        int fingers = 0, f;
        SDL_Finger **list = SDL_GetTouchFingers(devices[d], &fingers);
        for (f = 0; list && f < fingers; f++) {
            float x, y, dx, dy;
            SDL_RenderCoordinatesFromWindow(renderer, list[f]->x * (float)window_w, list[f]->y * (float)window_h, &x, &y);
            dx = x - pad_x;
            dy = y - pad_y;
            if (dx * dx + dy * dy < (pad_radius + 14) * (pad_radius + 14)) {    /* the pad: eight directions */
                if (dx > dead && dx > SDL_fabsf(dy) * 0.45f) held |= 1 << 4;
                if (dx < -dead && -dx > SDL_fabsf(dy) * 0.45f) held |= 1 << 5;
                if (dy < -dead && -dy > SDL_fabsf(dx) * 0.45f) held |= 1 << 6;
                if (dy > dead && dy > SDL_fabsf(dx) * 0.45f) held |= 1 << 7;
            }
            for (i = 0; i < n; i++)
                if (x >= buttons[i].x - 4 && x < buttons[i].x + buttons[i].w + 4 && y >= buttons[i].y - 4 && y < buttons[i].y + buttons[i].h + 4)
                    held |= (uint16_t)(1 << buttons[i].bit);
        }
        SDL_free(list);
    }
    SDL_free(devices);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (i = 0; i < n; i++) {
        int on = (held >> buttons[i].bit) & 1;
        box = (SDL_FRect){ buttons[i].x, buttons[i].y, buttons[i].w, buttons[i].h };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, on ? 110 : 45);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
        SDL_RenderRect(renderer, &box);
    }
    for (i = 0; i < 4; i++) {                       /* the pad as a cross: right, left, up, down */
        static const float ox[4] = { 14, -36, -11, -11 }, oy[4] = { -11, -11, -36, 14 };
        int on = (held >> (4 + i)) & 1;
        box = (SDL_FRect){ pad_x + ox[i], pad_y + oy[i], 22, 22 };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, on ? 110 : 45);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
        SDL_RenderRect(renderer, &box);
    }
    return held;
}

/* Show one `width` x 160 ARGB frame, wait until the next 1/59.7275 s, and return the buttons held (bit order above). */
uint16_t ss_plat_present(const uint32_t *pixels, int width)
{
    static int shown_width;
    SDL_FRect source = { 0, 0, (float)width, SS_SCREEN_H };
    SDL_Rect update = { 0, 0, width, SS_SCREEN_H };
    const bool *state;
    uint16_t held = 0;
    uint64_t now;
    SDL_Event event;
    int i;

    if (!window)
        return 0;
    while (SDL_PollEvent(&event))
        handle_event(&event);
    state = SDL_GetKeyboardState(NULL);
    for (i = 0; i < 10; i++)
        if (state[keys[i]])
            held |= (uint16_t)(1 << i);
    if (gamepad) {
        /* the stick acts as the d-pad */
        int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        for (i = 0; i < 10; i++)
            if (SDL_GetGamepadButton(gamepad, pad_buttons[i]))
                held |= (uint16_t)(1 << i);
        if (x > 12000) held |= 1 << 4;
        if (x < -12000) held |= 1 << 5;
        if (y < -12000) held |= 1 << 6;
        if (y > 12000) held |= 1 << 7;
    }
    if ((held & 0x30) == 0x30) held &= ~0x30;       /* left+right or up+down cannot happen on the console's pad */
    if ((held & 0xC0) == 0xC0) held &= ~0xC0;

    if (classic < 0)
        read_mode();
    if (width != shown_width) {
        if (classic)
            SDL_SetRenderLogicalPresentation(renderer, width + 2 * border, SS_SCREEN_H + 2 * border, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        else
            SDL_SetRenderLogicalPresentation(renderer, width, SS_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        shown_width = width;
    }
    SDL_UpdateTexture(texture, &update, pixels, width * 4);
    if (classic) {
        SDL_FRect inside = { (float)border, (float)border, (float)width, SS_SCREEN_H };
        draw_border();
        SDL_RenderTexture(renderer, texture, &source, &inside);
        draw_overlay(width + 2 * border, SS_SCREEN_H + 2 * border);
        draw_notice(border, border, width);
        held |= touch_buttons(width + 2 * border);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        /* Enhanced mode: where the picture has nothing to show (beside a menu, past the edge of a map) the
         * renderer leaves the pixels transparent and the port's field shows through. SS_SIDES=0 keeps black. */
        if (width > SS_SCREEN_W && sides_field)
            draw_field(width, SS_SCREEN_H, 0, 0, 0, 0);
        SDL_RenderTexture(renderer, texture, &source, NULL);
        draw_overlay(width, SS_SCREEN_H);
        draw_notice(0, 0, width);
        held |= touch_buttons(width);
    }
    {                                   /* SS_WINDOW_SHOT=N: save what the window shows at presented frame N */
        static long shot_at = -1, presented;
        if (shot_at < 0)
            shot_at = getenv("SS_WINDOW_SHOT") ? atol(getenv("SS_WINDOW_SHOT")) : 0;
        if (shot_at && ++presented == shot_at) {
            SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
            if (surface) {
                SDL_SaveBMP(surface, "ss_window.bmp");
                SDL_DestroySurface(surface);
            }
        }
    }
    SDL_RenderPresent(renderer);

    /* the console refreshes at 59.7275 Hz; game speed must not depend on the monitor */
    next_frame_ns += 16742706;
    now = SDL_GetTicksNS();
    if (display_paced && !overlay_skipping) {
        next_frame_ns = now;                        /* the display's refresh already spaced this frame */
    } else if (next_frame_ns > now) {
        SDL_DelayPrecise(next_frame_ns - now);
        stat_waits++;
        stat_wait_ns += next_frame_ns - now;
    }
    else if (now - next_frame_ns > 100000000)
        next_frame_ns = now;                        /* fell far behind (debugger, window drag): do not catch up */
    if (stat_interval_us) {
        uint64_t shown = SDL_GetTicksNS();
        if (stat_last_ns)
            stat_interval_us[stat_count++ % STAT_SLOTS] = (uint32_t)((shown - stat_last_ns) / 1000);
        stat_last_ns = shown;
    }
    return held;
}
