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
static void read_bindings(const char *name, int *out, int limit)
{
    const char *text = getenv(name);
    int i;
    for (i = 0; text && *text && i < 10; i++) {
        char *end;
        long value = strtol(text, &end, 10);
        if (end == text)
            break;
        if (value > 0 && value < limit)
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
    read_bindings("SS_KEYS", (int *)keys, SDL_SCANCODE_COUNT);
    read_bindings("SS_PAD", (int *)pad_buttons, SDL_GAMEPAD_BUTTON_COUNT);
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
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SS_MAX_VIEW_W, SS_SCREEN_H);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
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
int ss_plat_view_width(void)
{
    int w = 0, h = 0, scale, width;
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

    if (width != shown_width) {
        SDL_SetRenderLogicalPresentation(renderer, width, SS_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        shown_width = width;
    }
    SDL_UpdateTexture(texture, &update, pixels, width * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, &source, NULL);
    held |= touch_buttons(width);
    SDL_RenderPresent(renderer);

    /* the console refreshes at 59.7275 Hz; game speed must not depend on the monitor */
    next_frame_ns += 16742706;
    now = SDL_GetTicksNS();
    if (next_frame_ns > now) {
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
