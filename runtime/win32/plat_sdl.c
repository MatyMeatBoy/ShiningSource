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
 * 59.7275 Hz and the display follows. 0 turns synchronisation off. Online is no different: the players' machines
 * do not step together (core/netlink.c, direct transport), so each may follow its own display. (Until 0.6 a
 * network address being set kept the game on its own clock; with online on by default that was everybody, and
 * a 60 Hz display then repeated a frame every few seconds.) */
static int display_paced;

/* ---- settings the options menu owns ----
 * Each has an environment variable (the launcher and the tests set those) and a key in shiningsource.ini, the
 * launcher's settings file in the working folder ("key=value" lines). At start the variable decides when it is
 * set, else the file, else the default; the menu writes what it changes back to the file (see "options menu"). */
enum { SET_MODE, SET_BORDER, SET_STYLE, SET_SIDES, SET_FULLSCREEN, SET_VSYNC, SET_VOLUME, SET_SMOOTH, SET_SKIP, SET_COUNT };
static const char *const setting_keys[SET_COUNT] = { "mode", "border", "border_style", "sides", "fullscreen", "vsync", "volume", "smooth", "skip" };
static const char *const setting_variables[SET_COUNT] = { "SS_MODE", "SS_BORDER", "SS_BORDER_STYLE", "SS_SIDES", "SS_FULLSCREEN", "SS_VSYNC",
                                                          "SS_VOLUME", "SS_SMOOTH", "SS_SKIP" };
static char setting_file[SET_COUNT][16];        /* what the file said at start ("" if nothing) */
static int option_vsync = 1, option_volume = 100, option_smooth = 1, option_skip = 1, option_fullscreen, vsync_refused;
static int view_forced;                         /* SS_VIEW: the enhanced picture's width, 0 to follow the window */

static void read_setting_file(void)
{
    static int done;
    FILE *file;
    char line[2048];
    int i;
    if (done)
        return;
    done = 1;
    file = fopen("shiningsource.ini", "rb");
    while (file && fgets(line, sizeof line, file)) {
        char *equals = strchr(line, '='), *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
            *--end = 0;
        if (!equals)
            continue;
        *equals = 0;
        for (i = 0; i < SET_COUNT; i++)
            if (!strcmp(line, setting_keys[i]))
                SDL_snprintf(setting_file[i], sizeof setting_file[i], "%s", equals + 1);
    }
    if (file)
        fclose(file);
#ifdef __ANDROID__
    setting_file[SET_MODE][0] = 0;      /* there the app's first screen asks for the mode at every start */
#endif
}

/* The text of a setting at start, or NULL when neither the environment nor the file has it. */
static const char *setting_text(int which)
{
    const char *text = getenv(setting_variables[which]);
    read_setting_file();
    return text ? text : setting_file[which][0] ? setting_file[which] : NULL;
}

static int classic = -1, border, border_style, sides_field = 1;     /* screen mode, see "screen modes" */
static void read_mode(void);
enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_OK, MENU_BACK, MENU_TOGGLE };
static int menu_open;
static void menu_command(int command);
static void menu_click(float x, float y, int pressed);
static void set_vsync(int wanted);
void (*ss_plat_idle)(void);     /* called while the game waits behind the menu, if the host wants to know */

/* ---- measurements (SS_STATS=1): how regular the frames were and how much memory the process used ----
 * Printed when the program ends. Intervals are the real time between two presented frames; the console's is
 * 16.74 ms. Late frames are those more than 2 ms over it. */
enum { STAT_SLOTS = 1 << 16 };
static uint32_t *stat_interval_us;
static unsigned stat_count;
static uint64_t stat_last_ns, stat_waits, stat_wait_ns;
/* the parts of presenting a frame, each in microseconds: the texture upload, the drawing calls, the present call
 * (which waits for the display when it is synchronised) */
enum { PART_UPLOAD, PART_DRAW, PART_PRESENT, PARTS };
static uint32_t *stat_part_us[PARTS];
static unsigned stat_part_count;
/* the sound queue as each frame's samples arrive: how full, how often empty (the device ran dry) and how often
 * full (the frame's sound was dropped) */
static unsigned long stat_audio_frames, stat_audio_dropped, stat_audio_empty, stat_audio_trimmed;
static uint64_t stat_audio_queued;
static int stat_audio_least = -1, stat_audio_most;
static double stat_first_frame_ms = -1;     /* from the start of the process to the first picture in the window */
static double stat_start_ms[5];             /* when each step of the window's start was done */

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

/* Milliseconds since this process was created (elsewhere: since the platform layer started). */
static double process_age_ms(void)
{
#ifdef _WIN32
    FILETIME created, ended, kernel, user, now;
    ULARGE_INTEGER from, to;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        return -1;
    GetSystemTimeAsFileTime(&now);
    from.LowPart = created.dwLowDateTime; from.HighPart = created.dwHighDateTime;
    to.LowPart = now.dwLowDateTime; to.HighPart = now.dwHighDateTime;
    return (double)(to.QuadPart - from.QuadPart) / 10000.0;
#else
    return (double)SDL_GetTicksNS() / 1e6;
#endif
}

static int by_value(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t stat_game_first_ns, stat_game_last_ns;
static unsigned long stat_left_out, stat_game_frames;

static void stats_report(void)
{
    unsigned n = stat_count < STAT_SLOTS ? stat_count : STAT_SLOTS, i, late = 0, early = 0;
    uint64_t sum = 0;
    if (stat_first_frame_ms >= 0)
        fprintf(stderr, "stats: first picture %.0f ms after the process started: %.0f before the window code (ROM, image), SDL %.0f, "
                "sound device %.0f, window and renderer %.0f, shown %.0f, first frame %.0f\n", stat_first_frame_ms, stat_start_ms[0],
                stat_start_ms[1] - stat_start_ms[0], stat_start_ms[2] - stat_start_ms[1], stat_start_ms[3] - stat_start_ms[2],
                stat_start_ms[4] - stat_start_ms[3], stat_first_frame_ms - stat_start_ms[4]);
    if (n > 60) {
        for (i = 0; i < n; i++) {
            sum += stat_interval_us[i];
            late += stat_interval_us[i] > 16743 + 2000;
            early += stat_interval_us[i] < 16743 - 2000;
        }
        qsort(stat_interval_us, n, sizeof *stat_interval_us, by_value);
        fprintf(stderr, "stats: %u frames, interval mean %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f; %u late (%.2f%%)\n",
                n, sum / 1000.0 / n, stat_interval_us[n / 2] / 1000.0, stat_interval_us[n * 95 / 100] / 1000.0,
                stat_interval_us[n * 99 / 100] / 1000.0, stat_interval_us[n - 1] / 1000.0, late, late * 100.0 / n);
        fprintf(stderr, "stats: shortest interval %.2f ms; %u more than 2 ms early (%.2f%%)\n", stat_interval_us[0] / 1000.0, early,
                early * 100.0 / n);
        fprintf(stderr, "stats: waited for the next frame %.1f%% of the time (the rest is the game, the picture and the system)\n",
                stat_wait_ns * 100.0 / (sum * 1000.0));
    }
    if (stat_game_frames > 1 && stat_game_last_ns > stat_game_first_ns)
        fprintf(stderr, "stats: %lu game frames in %.2f s: %.2f a second (the console: 59.73); %lu of them without a picture\n",
                stat_game_frames, (stat_game_last_ns - stat_game_first_ns) / 1e9,
                (stat_game_frames - 1) * 1e9 / (double)(stat_game_last_ns - stat_game_first_ns), stat_left_out);
    n = stat_part_count < STAT_SLOTS ? stat_part_count : STAT_SLOTS;
    for (i = 0; n > 60 && i < PARTS; i++) {
        static const char *const names[PARTS] = { "texture upload", "drawing calls", "present call" };
        unsigned k;
        for (sum = 0, k = 0; k < n; k++)
            sum += stat_part_us[i][k];
        qsort(stat_part_us[i], n, sizeof *stat_part_us[i], by_value);
        fprintf(stderr, "stats: %s: mean %.0f us, p50 %u, p99 %u, max %u\n", names[i], (double)sum / n, (unsigned)stat_part_us[i][n / 2],
                (unsigned)stat_part_us[i][n * 99 / 100], (unsigned)stat_part_us[i][n - 1]);
    }
    if (stat_audio_frames)
        fprintf(stderr, "stats: sound queue when a frame's samples arrive: mean %.1f ms, least %.1f, most %.1f; empty %lu times, "
                "dropped %lu of %lu frames; speed trimmed in %lu\n", stat_audio_queued / (double)stat_audio_frames / 192.0,
                stat_audio_least / 192.0, stat_audio_most / 192.0, stat_audio_empty, stat_audio_dropped, stat_audio_frames, stat_audio_trimmed);
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

/* The picture's texture. Made at the start, and again when the system says the graphics device was reset (a
 * driver restart, an Android app coming back without its graphics context): every texture is gone then. The
 * others are made again where they are used: the typeface from the ROM, a notice's icon from its pixels. */
static SDL_Texture *font_texture, *notice_texture, *field_texture;
static int font_state, texture_remade;

static void make_textures(void)
{
    if (texture)
        SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SS_MAX_VIEW_W, SS_SCREEN_H);
    if (!texture)
        fprintf(stderr, "window: no texture for the picture: %s\n", SDL_GetError());
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);      /* transparent pixels let the side field show */
    if (font_texture)
        SDL_DestroyTexture(font_texture);
    font_texture = NULL;
    font_state = 0;                     /* looked for again at the next text */
    if (notice_texture)
        SDL_DestroyTexture(notice_texture);
    notice_texture = NULL;
    if (field_texture)
        SDL_DestroyTexture(field_texture);
    field_texture = NULL;
    texture_remade = 1;                 /* the frame at hand is uploaded again (ss_plat_present) */
}

/* For a host that ends the process without running what atexit registered (Android): the statistics, if asked for. */
void ss_plat_last_words(void)
{
    if (stat_interval_us)
        stats_report();
}

int ss_plat_video_init(const char *title)
{
    SDL_AudioSpec spec;

#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");     /* otherwise a resizable window follows the user */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");                    /* Back is a key here: it opens the options menu */
#endif
    stat_start_ms[0] = process_age_ms();
    read_bindings("SS_KEYS", (int *)keys, 1, SDL_SCANCODE_COUNT);
    read_bindings("SS_PAD", (int *)pad_buttons, 0, SDL_GAMEPAD_BUTTON_COUNT);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    stat_start_ms[1] = process_age_ms();
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 48000;
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    {
        const char *volume = setting_text(SET_VOLUME);      /* 0..100, set by the launcher or the menu */
        option_volume = volume ? atoi(volume) : 100;
        if (option_volume < 0) option_volume = 0;
        if (option_volume > 100) option_volume = 100;
    }
    if (audio) {
        if (option_volume != 100)
            SDL_SetAudioStreamGain(audio, (float)option_volume / 100.0f);
        SDL_ResumeAudioStreamDevice(audio);
    }
    else
        fprintf(stderr, "no audio device: %s\n", SDL_GetError());
    stat_start_ms[2] = process_age_ms();
    if (!SDL_CreateWindowAndRenderer(title, SS_SCREEN_W * 4, SS_SCREEN_H * 4, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    {
        const char *text = setting_text(SET_VSYNC);
        option_vsync = text ? atoi(text) : 1;
        if (option_vsync < 0 || option_vsync > 2) option_vsync = 1;
        set_vsync(option_vsync);
    }
    make_textures();
    stat_start_ms[3] = process_age_ms();
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(window, true);
#else
    /* the variable's presence is the launcher's "yes"; the file says 1 or 0 */
    option_fullscreen = getenv("SS_FULLSCREEN") || (setting_text(SET_FULLSCREEN) && atoi(setting_text(SET_FULLSCREEN)));
    if (option_fullscreen)
        SDL_SetWindowFullscreen(window, true);
#endif
    {
        /* Two of the settings live in the core, which reads its variables by itself: it is only told when the
         * file decides. The picture's width is the core's too (SS_VIEW, and 240 when SS_MODE says classic): when
         * the file made the mode classic the core is told to follow this file's answer, which is then 240. */
        const char *text = getenv("SS_VIEW");
        view_forced = text ? atoi(text) : 0;
        if (view_forced && view_forced < SS_SCREEN_W) view_forced = SS_SCREEN_W;
        if (view_forced > SS_MAX_VIEW_W) view_forced = SS_MAX_VIEW_W;
        text = setting_text(SET_SMOOTH);
        option_smooth = !(text && text[0] == '0');
        if (!getenv("SS_SMOOTH") && text)
            ss_frame_set_smooth(option_smooth);
        text = setting_text(SET_SKIP);
        option_skip = !(text && text[0] == '0');
        if (!getenv("SS_SKIP") && text)
            ss_skip_set_enabled(option_skip);
        if (classic < 0)
            read_mode();
        if (classic && !getenv("SS_MODE"))
            ss_frame_set_view(0);
    }
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);                /* come to the front even when started from a console or a dialog */
    stat_start_ms[4] = process_age_ms();
    next_frame_ns = SDL_GetTicksNS();
    if (getenv("SS_STATS")) {
        int part;
        stat_interval_us = calloc(STAT_SLOTS, sizeof *stat_interval_us);
        for (part = 0; part < PARTS; part++)
            stat_part_us[part] = calloc(STAT_SLOTS, sizeof *stat_part_us[part]);
        atexit(stats_report);
    }
    return 0;
}

/* Decide who keeps time, see display_paced. `wanted` is the setting: 0 nobody synchronises, 1 VSync, 2 adaptive
 * (variable refresh). Called at start, by the options menu, and when the window finds itself on another display
 * or its display changes mode; SDL lets the renderer change it while it runs. What VSync means depends on the
 * display's refresh rate:
 *   a whole multiple of about 60 Hz (60, 120, 180, 240): the display paces the game;
 *   any other rate the game's 59.7275 Hz fits in (75, 90, 100, 144, 165, 360): the game keeps its own clock and
 *     every picture waits for the display's next refresh, which is always sooner than the next frame is due;
 *   a display slower than the game (50 Hz, 30 Hz): waiting for its refresh would slow the game to the display's
 *     rate and starve the sound, so nothing waits for it: the game keeps its clock and full speed, and the
 *     picture may tear. (The other way, keeping the wait and dropping pictures, would need a present that
 *     returns at once when the display is not ready, which SDL's renderer does not offer.)
 * A display or a driver without adaptive synchronisation gets none, and the menu says so.
 * SS_TEST_REFRESH=N makes this believe the display runs at N Hz (tests of the decision on one monitor). */
static float audio_ratio = 1.0f;        /* how much faster than recorded the sound is played: the display's pace */
static int audio_trim;                  /* -1, 0, 1: played 0.4% slower, as it is, 0.4% faster (ss_plat_audio) */

static void set_vsync(int wanted)
{
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    float refresh = mode && mode->refresh_rate > 1.0f ? mode->refresh_rate : 60.0f, rate = 59.7275f;
    const char *said = "no vsync";
    int every;
    if (getenv("SS_TEST_REFRESH") && atof(getenv("SS_TEST_REFRESH")) > 1.0)
        refresh = (float)atof(getenv("SS_TEST_REFRESH"));
    every = (int)(refresh / 60.0f + 0.5f);
    display_paced = 0;
    vsync_refused = 0;
    if (wanted == 2) {
        said = "variable refresh, own pacing";
        if (!SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE)) {
            SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
            vsync_refused = 1;
            said = "variable refresh not available: no vsync, own pacing";
        }
    } else if (wanted == 1) {
        rate = every > 0 ? refresh / (float)every : 0.0f;
        if (every >= 1 && rate > 58.5f && rate < 61.5f && SDL_SetRenderVSync(renderer, every)) {
            display_paced = 1;
            said = "paced by the display";
        } else if (refresh < 59.0f) {
            SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
            said = "slower than the game: no vsync, own pacing";
        } else {
            SDL_SetRenderVSync(renderer, 1);
            said = "vsync, own pacing";
        }
    } else {
        SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
    }
    audio_ratio = display_paced ? rate / 59.7275f : 1.0f;
    audio_trim = 0;
    if (audio)
        SDL_SetAudioStreamFrequencyRatio(audio, audio_ratio);
    next_frame_ns = SDL_GetTicksNS();
    fprintf(stderr, "video: display %.2f Hz, %s\n", refresh, said);
}

void ss_plat_audio(const int16_t *samples, int frames)
{
    int queued = audio ? SDL_GetAudioStreamQueued(audio) : 0;
    if (audio && stat_interval_us) {
        if (stat_audio_frames > 30) {           /* after the first half second: the queue starts empty */
            stat_audio_queued += (uint64_t)queued;
            stat_audio_empty += queued == 0;
            if (stat_audio_least < 0 || queued < stat_audio_least) stat_audio_least = queued;
            if (queued > stat_audio_most) stat_audio_most = queued;
        }
        stat_audio_dropped += queued >= 48000 * 4 / 10;
        stat_audio_frames++;
    }
    if (!audio)
        return;
    /* never let more than about a tenth of a second queue up: late sound is worse than a dropped frame */
    if (queued >= 48000 * 4 / 10)
        return;
    {
        /* The device takes sound at its own steady pace; frames bring it a frame at a time, now and then late.
         * With nothing in reserve every late frame is a gap. So: when the queue has run dry (the start, the
         * pause behind the menu, a skipped scene, a frame that took too long) a reserve of silence goes in first,
         * once; and because the device's clock and the game's are never quite the same, the reserve is held: when
         * its average sinks under 20 ms the sound is played 0.4% slower, over 55 ms 0.4% faster, until it is back
         * near the middle. That is a few cents of pitch, and most of the time nothing is changed at all. */
        enum { MS = 48000 * 4 / 1000, RESERVE = 36 * MS, LOW = 20 * MS, HIGH = 55 * MS };
        static const int16_t silence[RESERVE / 2];
        static int level = RESERVE;
        static uint64_t last_ns;
        uint64_t now_ns = SDL_GetTicksNS();
        int trim = audio_trim;
        if (queued <= 0 && now_ns - last_ns > 100000000) {
            SDL_PutAudioStreamData(audio, silence, RESERVE);
            queued = level = RESERVE;
        }
        last_ns = now_ns;
        level += (queued - level) / 16;
        if (level < LOW) trim = -1;
        else if (level > HIGH) trim = 1;
        else if ((trim < 0 && level >= RESERVE) || (trim > 0 && level <= RESERVE)) trim = 0;
        if (trim != audio_trim) {
            audio_trim = trim;
            SDL_SetAudioStreamFrequencyRatio(audio, audio_ratio * (1.0f + 0.004f * (float)trim));
        }
        if (stat_interval_us)
            stat_audio_trimmed += trim != 0;
    }
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
static void read_mode(void)
{
    const char *text = setting_text(SET_MODE);
    classic = text && !strcmp(text, "classic");
    border = setting_text(SET_BORDER) ? atoi(setting_text(SET_BORDER)) : 16;
    if (border < 0) border = 0;
    if (border > 64) border = 64;
    sides_field = !(setting_text(SET_SIDES) && setting_text(SET_SIDES)[0] == '0');
    border_style = setting_text(SET_STYLE) ? atoi(setting_text(SET_STYLE)) : 4;
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
 * Nothing is drawn inside the given box (where the picture goes).
 * It is the same every frame, and drawn as rectangles it is about a thousand of them: so it is computed once for
 * a shape and kept as a texture (made again when the shape changes or the graphics device was reset). If the
 * texture cannot be made the rectangles are drawn as before. */
static int field_shape[6];              /* field_texture is declared with make_textures */

static void draw_field(int w, int h, int box_left, int box_top, int box_right, int box_bottom)
{
    const int shape[6] = { w, h, box_left, box_top, box_right, box_bottom };
    int x, y, k;
    if (!field_texture || memcmp(shape, field_shape, sizeof shape)) {
        uint32_t *pixels = w > 0 && h > 0 ? malloc((size_t)w * (size_t)h * 4) : NULL;
        if (field_texture)
            SDL_DestroyTexture(field_texture);
        field_texture = pixels ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h) : NULL;
        if (field_texture) {
            for (y = 0; y < h; y++) {
                int t = y * 255 / (h > 1 ? h - 1 : 1);
                uint32_t colour = 0xFF000000u | (uint32_t)(20 - t * 10 / 255) << 16 | (uint32_t)(62 - t * 34 / 255) << 8 | (uint32_t)(74 - t * 36 / 255);
                for (x = 0; x < w; x++)
                    pixels[y * w + x] = colour;
            }
            for (y = 3; y < h; y += 7)
                for (x = 3 + (y * 5) % 11; x < w; x += 13) {
                    if (x >= box_left && x < box_right && y >= box_top && y < box_bottom)
                        continue;
                    k = (x * 31 + y * 17) % 5;
                    pixels[y * w + x] = 0xFF000000u | (uint32_t)(60 + k * 22) << 16 | (uint32_t)(120 + k * 20) << 8 | (uint32_t)(130 + k * 20);
                }
            if (SDL_UpdateTexture(field_texture, NULL, pixels, w * 4)) {
                SDL_SetTextureBlendMode(field_texture, SDL_BLENDMODE_NONE);
                SDL_SetTextureScaleMode(field_texture, SDL_SCALEMODE_NEAREST);
                memcpy(field_shape, shape, sizeof shape);
            } else {
                SDL_DestroyTexture(field_texture);
                field_texture = NULL;
            }
        }
        free(pixels);
    }
    if (field_texture) {
        SDL_FRect whole = { 0, 0, (float)w, (float)h };
        if (SDL_RenderTexture(renderer, field_texture, NULL, &whole))
            return;
    }
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
    if (event->type == SDL_EVENT_QUIT)      /* closing the window (or Alt+F4); Escape opens the options menu */
        exit(0);
    if (event->type == SDL_EVENT_GAMEPAD_ADDED && !gamepad)
        gamepad = SDL_OpenGamepad(event->gdevice.which);
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED && gamepad && SDL_GetGamepadID(gamepad) == event->gdevice.which) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_F11)
        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    if (event->type == SDL_EVENT_RENDER_DEVICE_RESET)
        make_textures();
    /* the window went to another display, or its display changed mode: who keeps time is decided again */
    if (event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event->type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED
        || event->type == SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED || event->type == SDL_EVENT_WINDOW_RESTORED)
        set_vsync(option_vsync);            /* (restored: a minimised window is not paced by its display) */
    /* The options menu. Escape opens and closes it (on Android the system's Back, which SDL hands over as a
     * key), and so does a controller's Guide button unless it was given to one of the console's buttons. In the
     * menu the arrows and the keys bound to the pad move and change, Enter, Space or the A key choose, Backspace
     * or the B key go back. A press on an entry with the mouse or a finger chooses it (SDL turns a touch into
     * mouse events). The controller's pad and buttons are read once a frame, in menu_buttons. */
    if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Scancode code = event->key.scancode;
        if (code == SDL_SCANCODE_ESCAPE || code == SDL_SCANCODE_AC_BACK) {
            if (!event->key.repeat)
                menu_command(MENU_TOGGLE);
        } else if (menu_open) {
            if (code == SDL_SCANCODE_UP || code == keys[6]) menu_command(MENU_UP);
            else if (code == SDL_SCANCODE_DOWN || code == keys[7]) menu_command(MENU_DOWN);
            else if (code == SDL_SCANCODE_LEFT || code == keys[5]) menu_command(MENU_LEFT);
            else if (code == SDL_SCANCODE_RIGHT || code == keys[4]) menu_command(MENU_RIGHT);
            else if (event->key.repeat) return;
            else if (code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER || code == SDL_SCANCODE_SPACE || code == keys[0]) menu_command(MENU_OK);
            else if (code == SDL_SCANCODE_BACKSPACE || code == keys[1]) menu_command(MENU_BACK);
        }
    }
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
        int i, taken = 0;
        for (i = 0; i < 10; i++)
            taken |= pad_buttons[i] == SDL_GAMEPAD_BUTTON_GUIDE;
        if (!taken)
            menu_command(MENU_TOGGLE);
    }
    if (menu_open && (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_MOTION)) {
        int w = 1, h = 1, pw = 1, ph = 1, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        float x = down ? event->button.x : event->motion.x, y = down ? event->button.y : event->motion.y;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        if (!down || event->button.button == SDL_BUTTON_LEFT)
            menu_click(x * (float)pw / (float)(w > 0 ? w : 1), y * (float)ph / (float)(h > 0 ? h : 1), down);
    }
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

/* ---- text ----
 * The port's own lines (the options menu, "skip scene", the notices) are written with the game's typeface: the
 * letters the game itself draws its messages with, read from the user's ROM, which is mapped in this process at
 * the console's address. Nothing of it is in the program. Where they are, for the Shining Soul II cartridge this
 * was made for (found through the decompilation's text routine, sub_0801A3D0, and its font resource): the
 * "SSDT" archive at 0x08518938, whose section 4 holds the fonts; the first is 256 cells of 128 bytes, each two
 * 8 x 8 tiles of four bits a pixel, one above the other (the second half of a cell is unused): 0 nothing,
 * 1 the letter, 3 its dark outline. The game gives every character eight columns; here the empty columns of a
 * cell are dropped and neighbours share their outline, which reads better in a line of prose.
 * Any other ROM, or one that does not look as expected, gets the 5 x 7 dots above instead, capitals only. */
/* font_texture (declared with make_textures): the cells in a 16 x 16 grid, letters white, outline dark;
 * font_state: 0 not looked for, 1 found, -1 not there */
static uint8_t font_left[256], font_wide[256];
static int block_text;                  /* 1: use the 5 x 7 dots even with the typeface (a very small window) */

static uint32_t rom_word(const uint8_t *at)
{
    return at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static void font_find(void)
{
    enum { ROM_BYTES = 0x1000000, ARCHIVE = 0x518938, CELL = 128, CELLS = 256 };
    static uint32_t pixels[CELLS * 8 * 16];
    const uint8_t *rom = (const uint8_t *)(uintptr_t)SS_ROM, *cells;
    uint32_t section, font;
    int cell, x, y;

    font_state = -1;
    if (getenv("SS_FONT") && getenv("SS_FONT")[0] == '0')       /* SS_FONT=0: the 5 x 7 dots, whatever the ROM */
        return;
    if (memcmp(rom + 0xAC, "AU2P", 4) || memcmp(rom + ARCHIVE, "SSDT", 4))
        return;
    section = ARCHIVE + rom_word(rom + ARCHIVE + 8 + 4 * 4);
    if (section < ARCHIVE || section > ROM_BYTES - 8 || rom_word(rom + section) < 1)
        return;
    font = section + rom_word(rom + section + 4);
    if (font < section || font > ROM_BYTES - CELL * CELLS)
        return;
    cells = rom + font;
    for (x = 0; x < 64; x++)
        if (cells[0x50 * CELL + x])
            return;                     /* the space is not empty: not the font */
    for (cell = 0; cell < CELLS; cell++) {
        int first = 8, last = -1;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 8; x++) {
                int value = (cells[cell * CELL + y * 4 + x / 2] >> (x & 1 ? 4 : 0)) & 15;
                pixels[((cell / 16) * 16 + y) * 128 + (cell % 16) * 8 + x] = !value ? 0 : value == 1 ? 0xFFFFFFFFu : 0xFF181C30u;
                if (value && x < first) first = x;
                if (value && x > last) last = x;
            }
        font_left[cell] = (uint8_t)(last < 0 ? 0 : first);
        font_wide[cell] = (uint8_t)(last < 0 ? 0 : last - first + 1);
    }
    if (!font_wide[0] || !font_wide[0x20])       /* A and a */
        return;
    font_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 128, 256);
    if (!font_texture)
        return;
    SDL_UpdateTexture(font_texture, NULL, pixels, 128 * 4);
    SDL_SetTextureBlendMode(font_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(font_texture, SDL_SCALEMODE_NEAREST);
    font_state = 1;
}

/* 1 when the game's typeface is used for the next text. */
static int text_game(void)
{
    if (!font_state && renderer)
        font_find();
    return font_state > 0 && !block_text;
}

/* The next character of a line and how many bytes it took. The text is UTF-8; a byte that is not gives the
 * Latin-1 character of that value. */
static unsigned text_next(const char *text, int *bytes)
{
    const unsigned char *s = (const unsigned char *)text;
    *bytes = 1;
    if (s[0] < 0x80)
        return s[0];
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *bytes = 2;
        return (unsigned)(s[0] & 0x1F) << 6 | (s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *bytes = 3;
        return (unsigned)(s[0] & 0x0F) << 12 | (unsigned)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *bytes = 4;
        return 0xFFFD;
    }
    return s[0];
}

/* The cell of the game's font for a character (the game's own character code less one), or -1 for a space. */
static int font_cell(unsigned c)
{
    /* the game's codes 0x61..0x79: capitals with accents in its own order, then C cedilla, N tilde, sharp s and
     * the two Spanish opening marks; the small letters follow 0x20 further on */
    static const uint8_t accented[25] = { 0xC0, 0xC1, 0xC4, 0xC2, 0xC8, 0xC9, 0xCB, 0xCA, 0xCC, 0xCD, 0xCE, 0xCF, 0xD2, 0xD3, 0xD6, 0xD4,
                                          0xD9, 0xDA, 0xDC, 0xDB, 0xC7, 0xD1, 0xDF, 0xA1, 0xBF };
    static const char marks[] = "<>;:@'", more[] = "!?%&[]", last[] = "/+-=.,";
    const char *found;
    int i;
    if (c >= 'A' && c <= 'Z') return (int)c - 0x40 - 1;
    if (c >= 'a' && c <= 'z') return (int)c - 0x40 - 1;
    if (c >= '0' && c <= '9') return (int)c + 0x11 - 1;
    if (c == ' ' || c == 0xA0) return -1;
    if (c < 0x80) {
        if ((found = strchr(marks, (int)c)) != NULL) return 0x1B + (int)(found - marks) - 1;
        if ((found = strchr(more, (int)c)) != NULL) return 0x3B + (int)(found - more) - 1;
        if ((found = strchr(last, (int)c)) != NULL) return 0x4B + (int)(found - last) - 1;
        switch (c) {
        case '*': return 0x52 - 1;
        case '"': return 0x53 - 1;
        case '$': return 0x55 - 1;
        case '(': return 0x56 - 1;
        case ')': return 0x57 - 1;
        case '|': return 0x7A - 1;
        case '`': return 0x7B - 1;
        case '^': return 0x7D - 1;
        case '_': return -1;
        }
        return 0x3C - 1;                /* the question mark */
    }
    if (c == 0x2026) return 0x59 - 1;   /* ... */
    if (c == 0x2019 || c == 0x2018) return 0x20 - 1;
    if (c == 0xB0) return 0x7C - 1;
    for (i = 0; i < 25; i++) {
        if (c == accented[i]) return 0x61 + i - 1;
        if (i < 22 && c == accented[i] + 0x20u) return 0x81 + i - 1;
    }
    return 0x3C - 1;
}

/* The same character for the 5 x 7 dots: a capital without its accent. */
static char block_char(unsigned c)
{
    /* U+00C0..U+00FF without the bit that tells capitals from small letters */
    static const char plain[33] = "AAAAAA?CEEEEIIII?NOOOOO?OUUUUY??";
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? (char)(c - 32) : c < 32 ? ' ' : (char)c;
    if (c == 0xA1) return '!';
    if (c == 0xBF) return '?';
    return c >= 0xC0 && c <= 0xFF ? plain[c & 0x1F] : c == 0xA0 ? ' ' : '?';
}

enum { FONT_SPACE = 4 };

static int text_height(void)
{
    return text_game() ? 16 : 7;
}

/* Width of the first `bytes` bytes of a line (the whole line when negative), in the units it is drawn in. */
static int text_width(const char *text, int bytes)
{
    int game = text_game(), width = 0, used;
    const char *end = bytes < 0 ? text + SDL_strlen(text) : text + bytes;
    while (text < end && *text) {
        unsigned c = text_next(text, &used);
        int cell = game ? font_cell(c) : 0;
        width += !game ? 6 : cell < 0 || !font_wide[cell] ? FONT_SPACE : font_wide[cell] - 1;
        text += used;
    }
    return width ? width + (game ? 1 : -1) : 0;
}

/* How many bytes of a line fit in `room` units (whole characters). */
static int text_fit(const char *text, int room)
{
    int bytes = 0, used;
    while (text[bytes]) {
        text_next(text + bytes, &used);
        if (text_width(text, bytes + used) > room)
            break;
        bytes += used;
    }
    return bytes;
}

/* One line of text with its top left corner at (x, y), in the given colour. */
static void draw_text(const char *text, int bytes, int x, int y, int r, int g, int b, int alpha)
{
    int game = text_game(), used, row, column;
    const char *end = bytes < 0 ? text + SDL_strlen(text) : text + bytes;
    SDL_FRect box, source;
    if (game) {
        SDL_SetTextureColorMod(font_texture, (Uint8)r, (Uint8)g, (Uint8)b);
        SDL_SetTextureAlphaMod(font_texture, (Uint8)alpha);
    } else {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)alpha);
    }
    while (text < end && *text) {
        unsigned c = text_next(text, &used);
        text += used;
        if (game) {
            int cell = font_cell(c);
            if (cell < 0 || !font_wide[cell]) {
                x += FONT_SPACE;
                continue;
            }
            source = (SDL_FRect){ (float)((cell % 16) * 8 + font_left[cell]), (float)((cell / 16) * 16), (float)font_wide[cell], 16 };
            box = (SDL_FRect){ (float)x, (float)y, (float)font_wide[cell], 16 };
            SDL_RenderTexture(renderer, font_texture, &source, &box);
            x += font_wide[cell] - 1;
        } else {
            const uint8_t *rows = glyph(block_char(c));
            for (row = 0; row < 7; row++)
                for (column = 0; column < 5; column++)
                    if (rows[row] & (0x10 >> column)) {
                        box = (SDL_FRect){ (float)(x + column), (float)(y + row), 1, 1 };
                        SDL_RenderFillRect(renderer, &box);
                    }
            x += 6;
        }
    }
}

/* Draw the overlay over a picture `width` x `height` units big (the current logical size). */
static void draw_overlay(int width, int height)
{
    SDL_FRect box;
    int length = (int)SDL_strlen(overlay_text), wide, tall, x, y;
    if (overlay_darkness > 0) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)overlay_darkness);
        box = (SDL_FRect){ 0, 0, (float)width, (float)height };
        SDL_RenderFillRect(renderer, &box);
    }
    if (!length)
        return;
    wide = text_width(overlay_text, -1);
    tall = text_height();
    x = (width - wide) / 2;
    y = height - 15 - tall;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    box = (SDL_FRect){ (float)x - 4, (float)y - 3, (float)(wide + 8), (float)(tall + 6) };
    SDL_RenderFillRect(renderer, &box);
    draw_text(overlay_text, -1, x, y, 255, 232, 150, 255);
}

/* ---- notices: a heading and one line, over the top of the picture for a few seconds ----
 * Used for achievements (runtime/achievements). They wait in a short queue, so two unlocked in the same frame are
 * both read. Time is counted in frames shown. A line longer than the picture slides to its end while it shows. */
enum { NOTICE_FRAMES = 300, NOTICE_FADE = 20, NOTICE_QUEUE = 16, NOTICE_ICON = 20, NOTICE_ICON_MAX = 128 };
struct notice { char heading[48], text[96]; int rebel, icon_size; uint32_t *icon; };    /* icon: square, ARGB, own copy */
static struct notice notices[NOTICE_QUEUE];
static int notice_count, notice_age;
/* notice_texture (declared with make_textures): the icon of the notice being shown */

/* The text is kept as it came (UTF-8); control characters become spaces. How it is written is decided when it is
 * drawn: the game's typeface has small letters and accents, the 5 x 7 dots have capitals only. */
static void notice_copy(char *out, size_t capacity, const char *text)
{
    size_t n = 0;
    for (; text && *text && n + 1 < capacity; text++)
        out[n++] = (unsigned char)*text < 32 ? ' ' : *text;
    while (n && ((unsigned char)out[n - 1] & 0xC0) == 0xC0)
        n--;                                        /* a character cut in two by the capacity */
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
    const char *line;
    int heading, heading_wide, shown, shown_wide, room, wide, tall, x, alpha = 255, icon, used, skip;
    SDL_FRect box;

    if (!notice_count)
        return;
    if (notice->icon && !notice_texture && (!notice_age || texture_remade)) {
        notice_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, notice->icon_size, notice->icon_size);
        if (notice_texture) {
            SDL_UpdateTexture(notice_texture, NULL, notice->icon, notice->icon_size * 4);
            SDL_SetTextureBlendMode(notice_texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(notice_texture, SDL_SCALEMODE_LINEAR);
        }
    }
    icon = notice_texture ? NOTICE_ICON + 4 : 0;    /* room at the left of the text */
    room = width - 16 - icon;
    tall = text_height();
    line = notice->text;
    if (notice_age < NOTICE_FADE)
        alpha = 255 * (notice_age + 1) / NOTICE_FADE;
    if (notice_age >= NOTICE_FRAMES - NOTICE_FADE)
        alpha = 255 * (NOTICE_FRAMES - notice_age) / NOTICE_FADE;
    heading = text_fit(notice->heading, room);
    heading_wide = text_width(notice->heading, heading);
    /* slide: a second still, then a character every eight frames until the end of the line is in sight */
    for (skip = (notice_age - 60) / 8; skip > 0 && text_width(line, -1) > room; skip--) {
        text_next(line, &used);
        line += used;
    }
    shown = text_fit(line, room);
    shown_wide = text_width(line, shown);
    wide = shown_wide > heading_wide ? shown_wide : heading_wide;
    x = left + (width - wide - icon) / 2 + icon;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(alpha * 190 / 255));
    box = (SDL_FRect){ (float)(x - 5 - icon), (float)top + 2, (float)(wide + 10 + icon), (float)(2 * tall + 10) };
    SDL_RenderFillRect(renderer, &box);
    if (notice->rebel)
        SDL_SetRenderDrawColor(renderer, 255, 96, 150, (Uint8)alpha);
    else
        SDL_SetRenderDrawColor(renderer, 232, 168, 56, (Uint8)alpha);
    SDL_RenderRect(renderer, &box);
    draw_text(notice->heading, heading, x + (wide - heading_wide) / 2, top + 5, notice->rebel ? 255 : 232, notice->rebel ? 96 : 168,
              notice->rebel ? 150 : 56, alpha);
    draw_text(line, shown, x + (wide - shown_wide) / 2, top + 8 + tall, 255, 244, 214, alpha);
    if (notice_texture) {
        box = (SDL_FRect){ (float)(x - 2 - icon), (float)(top + 2 + (2 * tall + 10 - NOTICE_ICON) / 2), NOTICE_ICON, NOTICE_ICON };
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
 * connected; SS_TOUCH=1 or 0 forces it. A small button at the top opens the options menu; while the menu is
 * open the buttons are neither drawn nor read (a touch then chooses in the menu, see handle_event). */
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
    static int wanted = -1, menu_before;
    const float pad_x = 40, pad_y = 116, pad_radius = 36, dead = 9;
    const SDL_FRect menu_box = { (float)width / 2 - 13, 2, 26, 13 };
    struct touch_button buttons[8];
    SDL_TouchID *devices;
    SDL_FRect box;
    uint16_t held = 0;
    int count = 0, d, i, n, window_w = 1, window_h = 1, menu_now = 0;

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
    if (menu_open) {
        menu_before = 1;                /* the finger that chose "resume" opens nothing when it is still down */
        return 0;
    }
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
            if (x >= menu_box.x - 4 && x < menu_box.x + menu_box.w + 4 && y >= menu_box.y - 2 && y < menu_box.y + menu_box.h + 4)
                menu_now = 1;
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
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 45);        /* the menu's button: three bars in a box */
    SDL_RenderFillRect(renderer, &menu_box);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
    SDL_RenderRect(renderer, &menu_box);
    for (i = 0; i < 3; i++) {
        box = (SDL_FRect){ menu_box.x + 7, menu_box.y + 3 + (float)i * 3, 12, 1 };
        SDL_RenderFillRect(renderer, &box);
    }
    if (menu_now && !menu_before)
        menu_command(MENU_TOGGLE);
    menu_before = menu_now;
    return menu_open ? 0 : held;
}

/* ---- options menu ----
 * Opened over the game with Escape, the system's Back on Android, Select and Start together on a controller (or
 * its Guide button), or the small button of the touch controls. Every entry takes effect at once and is written
 * to shiningsource.ini, so the launcher and the next start find it.
 *
 * While it is open the game gets no buttons. A game alone waits: ss_plat_present does not return, it keeps
 * drawing the same picture under the menu, and no sound is made because none is produced. Online nobody can
 * wait for one player, so the game goes on behind the menu and the menu says so.
 *
 * Tests: SS_TEST_KEYS="picture:key,..." plays the part of the keyboard, where `picture` counts what the window
 * has shown (as SS_WINDOW_SHOT does; game frames stop while the menu is open alone, pictures do not) and key is
 * one of esc up down left right ok back, pad:MASK (a controller holding those buttons from then on, mask as in
 * SS_INPUT), click:X:Y (a press at that window pixel), min and restore (the window is minimised, brought back), or
 * reset (the textures are made again, as when the graphics device was reset). With it set, what the menu does is
 * written to stderr. */
enum { ITEM_RESUME, ITEM_MODE, ITEM_BORDER, ITEM_STYLE, ITEM_SIDES, ITEM_FULLSCREEN, ITEM_VSYNC, ITEM_VOLUME, ITEM_SMOOTH, ITEM_SKIP,
       ITEM_QUIT, ITEM_KINDS };
enum { INFO_LINES = 8, INFO_BYTES = 96 };
static int menu_row, menu_quit_armed, menu_online;
static uint16_t menu_swallow;           /* buttons still held from the menu: not the game's until they are let go */
static unsigned settings_changed;       /* one bit per setting the menu changed in this session */
static long pictures, game_frames;      /* what the window has shown, and how many of those were new game frames */
static int test_log;
static uint16_t test_pad;
static int logical_w, logical_h;        /* the logical size the renderer has now (0: none, the menu's own scale) */
static struct { int scale, x, w, rows_y, pitch, count, items[ITEM_KINDS], middle[ITEM_KINDS], reach[ITEM_KINDS]; } menu_shape;

static int spanish(void)
{
    static int known = -1;
    if (known < 0) {
        const char *text = getenv("SS_LANG");
        known = text && text[0] == 'e' && text[1] == 's';
    }
    return known;
}

/* The entries shown now, top to bottom. */
static int menu_items(int *items)
{
    int n = 0;
    items[n++] = ITEM_RESUME;
    items[n++] = ITEM_MODE;
    if (classic) {
        items[n++] = ITEM_BORDER;
        items[n++] = ITEM_STYLE;
    } else {
        items[n++] = ITEM_SIDES;
    }
#ifndef __ANDROID__
    items[n++] = ITEM_FULLSCREEN;       /* an Android app always has the whole screen */
#endif
    items[n++] = ITEM_VSYNC;
    items[n++] = ITEM_VOLUME;
    items[n++] = ITEM_SMOOTH;
    items[n++] = ITEM_SKIP;
    items[n++] = ITEM_QUIT;
    return n;
}

static const char *item_label(int item)
{
    static const char *const names[ITEM_KINDS][2] = {
        { "Resume", "Continuar" }, { "Screen mode", "Modo de pantalla" }, { "Border size", "Grosor del borde" },
        { "Border style", "Aspecto del borde" }, { "Sides", "Lados" }, { "Full screen", "Pantalla completa" }, { "VSync", "VSync" },
        { "Volume", "Volumen" }, { "Smooth fast blinking", "Suavizar parpadeo r\xC3\xA1pido" },
        { "Skip scene with Start", "Saltar escena con Start" }, { "Quit game", "Salir del juego" },
    };
    if (item == ITEM_QUIT && menu_quit_armed)
        return spanish() ? "Pulsa otra vez para salir" : "Press again to quit";
    return names[item][spanish()];
}

/* The value an entry shows at its right, or NULL for one that only does something. */
static const char *item_value(int item, char *buffer, size_t capacity)
{
    static const char *const styles[5][2] = { { "Black", "Negro" }, { "Graphite", "Grafito" }, { "Indigo", "\xC3\x8Dndigo" },
                                              { "Silver", "Plata" }, { "Soul", "Soul" } };
    const int es = spanish();
    const char *on = es ? "S\xC3\xAD" : "On", *off = es ? "No" : "Off";
    switch (item) {
    case ITEM_MODE: return classic ? (es ? "Cl\xC3\xA1sico" : "Classic") : (es ? "Mejorado" : "Enhanced");
    case ITEM_BORDER: SDL_snprintf(buffer, capacity, "%d", border); return buffer;
    case ITEM_STYLE: return styles[border_style][es];
    case ITEM_SIDES: return sides_field ? (es ? "Campo" : "Field") : (es ? "Negro" : "Black");
    case ITEM_FULLSCREEN: return option_fullscreen ? on : off;
    case ITEM_VSYNC:
        if (option_vsync == 2)
            return vsync_refused ? (es ? "VRR (sin sincron\xC3\xAD" "a)" : "VRR (no sync)") : "VRR";
        return option_vsync ? on : off;
    case ITEM_VOLUME: SDL_snprintf(buffer, capacity, "%d%%", option_volume); return buffer;
    case ITEM_SMOOTH: return option_smooth ? on : off;
    case ITEM_SKIP: return option_skip ? on : off;
    }
    return NULL;
}

/* A setting as the settings file holds it (the launcher's words). */
static void setting_value(int which, char *out, size_t capacity)
{
    int number = which == SET_BORDER ? border : which == SET_STYLE ? border_style : which == SET_SIDES ? sides_field
                 : which == SET_FULLSCREEN ? option_fullscreen : which == SET_VSYNC ? option_vsync : which == SET_VOLUME ? option_volume
                 : which == SET_SMOOTH ? option_smooth : option_skip;
    if (which == SET_MODE)
        SDL_snprintf(out, capacity, "%s", classic ? "classic" : "enhanced");
    else
        SDL_snprintf(out, capacity, "%d", number);
}

/* Write what the menu changed to shiningsource.ini in the working folder: the launcher's file, "key=value"
 * lines. Only the lines of the keys changed here are replaced (or added at the end, or the file made, when they
 * are not there); every other line stays byte for byte. The new file is written beside the old one and takes its
 * place when it is complete. */
static void save_settings(void)
{
    static const char name[] = "shiningsource.ini", fresh[] = "shiningsource.ini.new";
    char *old = NULL, value[32];
    const char *line, *ending;
    long size = 0;
    unsigned written = 0;
    int i;
    FILE *file = fopen(name, "rb"), *out;

    if (file) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
        old = size >= 0 && size < (1 << 24) ? malloc((size_t)size + 1) : NULL;
        if (!old || fread(old, 1, (size_t)size, file) != (size_t)size) {
            fclose(file);
            free(old);
            return;                     /* a file that cannot be read whole is not replaced */
        }
        old[size] = 0;
        fclose(file);
    }
#ifdef _WIN32
    ending = old && size && !strstr(old, "\r\n") ? "\n" : "\r\n";      /* the launcher writes the system's line ends */
#else
    ending = old && strstr(old, "\r\n") ? "\r\n" : "\n";
#endif
    out = fopen(fresh, "wb");
    if (!out) {
        free(old);
        return;
    }
    for (line = old; line && *line; ) {
        const char *end = strchr(line, '\n'), *next = end ? end + 1 : line + strlen(line), *equals = memchr(line, '=', (size_t)(next - line));
        int which = -1;
        for (i = 0; equals && i < SET_COUNT; i++)
            if (strlen(setting_keys[i]) == (size_t)(equals - line) && !memcmp(line, setting_keys[i], (size_t)(equals - line)))
                which = i;
        if (which >= 0 && (settings_changed >> which & 1)) {
            if (!(written >> which & 1)) {          /* a second line of the same key is dropped */
                setting_value(which, value, sizeof value);
                fprintf(out, "%s=%s%s", setting_keys[which], value, end ? (end > line && end[-1] == '\r' ? "\r\n" : "\n") : ending);
                written |= 1u << which;
            }
        } else {
            fwrite(line, 1, (size_t)(next - line), out);
            if (!end && settings_changed & ~written)
                fputs(ending, out);                 /* the last line had no end and more follow */
        }
        line = next;
    }
    for (i = 0; i < SET_COUNT; i++)
        if ((settings_changed >> i & 1) && !(written >> i & 1)) {
            setting_value(i, value, sizeof value);
            fprintf(out, "%s=%s%s", setting_keys[i], value, ending);
        }
    free(old);
    if (ferror(out) | fclose(out)) {
        remove(fresh);
        return;
    }
#ifdef _WIN32
    if (!MoveFileExA(fresh, name, MOVEFILE_REPLACE_EXISTING))
        remove(fresh);
#else
    if (rename(fresh, name) != 0)
        remove(fresh);
#endif
}

/* Change an entry's value one step down (-1) or up (+1), at once, and remember it. */
static void item_change(int item, int step)
{
    char value[32];
    int which = -1;
    switch (item) {
    case ITEM_MODE:
        classic = !classic;
        ss_frame_set_view(classic ? 0 : view_forced);       /* classic: the core asks ss_plat_view_width, which says 240 */
        which = SET_MODE;
        break;
    case ITEM_BORDER:
        border = step > 0 ? (border / 4 + 1) * 4 : ((border + 3) / 4 - 1) * 4;
        if (border < 0) border = 0;
        if (border > 64) border = 64;
        which = SET_BORDER;
        break;
    case ITEM_STYLE:
        border_style = (border_style + step + 5) % 5;
        which = SET_STYLE;
        break;
    case ITEM_SIDES:
        sides_field = !sides_field;
        which = SET_SIDES;
        break;
    case ITEM_FULLSCREEN:
        option_fullscreen = !option_fullscreen;
        SDL_SetWindowFullscreen(window, option_fullscreen);
        which = SET_FULLSCREEN;
        break;
    case ITEM_VSYNC:
        option_vsync = (option_vsync + step + 3) % 3;
        set_vsync(option_vsync);
        which = SET_VSYNC;
        break;
    case ITEM_VOLUME:
        option_volume = step > 0 ? (option_volume / 10 + 1) * 10 : ((option_volume + 9) / 10 - 1) * 10;
        if (option_volume < 0) option_volume = 0;
        if (option_volume > 100) option_volume = 100;
        if (audio)
            SDL_SetAudioStreamGain(audio, (float)option_volume / 100.0f);
        which = SET_VOLUME;
        break;
    case ITEM_SMOOTH:
        option_smooth = !option_smooth;
        ss_frame_set_smooth(option_smooth);
        which = SET_SMOOTH;
        break;
    case ITEM_SKIP:
        option_skip = !option_skip;
        ss_skip_set_enabled(option_skip);
        which = SET_SKIP;
        break;
    }
    if (which < 0)
        return;
    settings_changed |= 1u << which;
    save_settings();
    if (test_log) {
        setting_value(which, value, sizeof value);
        fprintf(stderr, "menu: %s=%s (picture %ld, game frame %ld)\n", setting_keys[which], value, pictures, game_frames);
    }
}

static void menu_close(void)
{
    menu_open = 0;
    menu_swallow = 0x03FF;
    next_frame_ns = SDL_GetTicksNS();   /* the time spent here is not time the game has to make up */
    if (test_log)
        fprintf(stderr, "menu: closed at picture %ld, game frame %ld\n", pictures, game_frames);
}

static void menu_command(int command)
{
    int items[ITEM_KINDS], count = menu_items(items), item;
    char buffer[32];
    if (command == MENU_TOGGLE && !menu_open) {
        if (!window)
            return;
        menu_open = 1;
        menu_row = 0;
        menu_quit_armed = 0;
        option_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;      /* F11 may have changed it */
        if (test_log)
            fprintf(stderr, "menu: opened at picture %ld, game frame %ld\n", pictures, game_frames);
        return;
    }
    if (!menu_open)
        return;
    if (menu_row >= count)
        menu_row = count - 1;
    item = items[menu_row];
    switch (command) {
    case MENU_TOGGLE:
    case MENU_BACK:
        menu_close();
        break;
    case MENU_UP:
    case MENU_DOWN:
        menu_row = (menu_row + (command == MENU_UP ? count - 1 : 1)) % count;
        menu_quit_armed = 0;
        break;
    case MENU_LEFT:
    case MENU_RIGHT:
        if (item_value(item, buffer, sizeof buffer))
            item_change(item, command == MENU_LEFT ? -1 : 1);
        break;
    case MENU_OK:
        if (item == ITEM_RESUME) {
            menu_close();
        } else if (item == ITEM_QUIT) {
            if (menu_quit_armed) {
                if (test_log)
                    fprintf(stderr, "menu: quit chosen at picture %ld, game frame %ld\n", pictures, game_frames);
                exit(0);                /* as closing the window does */
            }
            menu_quit_armed = 1;        /* asked twice: leaving loses what was not saved in the game */
        } else {
            item_change(item, 1);
        }
        break;
    }
}

/* The pointer (mouse or finger) at a window pixel: it marks the entry under it, and a press chooses it. On an
 * entry with a value the press steps it up, or down when it falls on the left arrow or the left half of the
 * value. */
static void menu_click(float x, float y, int pressed)
{
    int ux, uy, row, item;
    char buffer[32];
    if (!menu_open || menu_shape.scale < 1)
        return;
    ux = (int)x / menu_shape.scale;
    uy = (int)y / menu_shape.scale;
    if (ux < menu_shape.x || ux >= menu_shape.x + menu_shape.w || uy < menu_shape.rows_y)
        return;
    row = (uy - menu_shape.rows_y) / menu_shape.pitch;
    if (row >= menu_shape.count)
        return;
    if (row != menu_row)
        menu_quit_armed = 0;
    menu_row = row;
    if (!pressed)
        return;
    item = menu_shape.items[row];
    if (item_value(item, buffer, sizeof buffer))
        item_change(item, ux < menu_shape.middle[row] && ux >= menu_shape.reach[row] ? -1 : 1);
    else
        menu_command(MENU_OK);
}

/* The controller in the menu, once per picture: the pad or the stick move and change (held, they repeat), the
 * console's A chooses and B goes back. `pad` are the buttons the controller holds. */
static void menu_buttons(uint16_t pad)
{
    static const int commands[4] = { MENU_RIGHT, MENU_LEFT, MENU_UP, MENU_DOWN };
    static uint16_t before;
    static int held_for;
    uint16_t pressed = (uint16_t)(pad & ~before);
    int i;
    before = pad;
    if (!menu_open)
        return;
    held_for = (pad & 0xF0) ? held_for + 1 : 0;
    for (i = 0; i < 4; i++)
        if ((pressed >> (4 + i) & 1) || ((pad >> (4 + i) & 1) && held_for >= 24 && held_for % 6 == 0))
            menu_command(commands[i]);
    if (pressed & 1)
        menu_command(MENU_OK);
    else if (pressed & 2)
        menu_command(MENU_BACK);
}

/* Select and Start together on the controller open and close the menu. So that neither button of that press
 * reaches the game, what the controller says about those two is handed on three pictures late (a twentieth of
 * a second, not felt on Start alone): when the second arrives within that time the first has not been seen by
 * the game, and both stay hidden until both are let go. Pressed further apart than that, the menu still opens,
 * but the game has seen the first button. */
static uint16_t menu_chord(uint16_t pad)
{
    enum { LATE = 3, BOTH = 0x000C };
    static uint16_t past[LATE];
    static int hidden, at;
    uint16_t late;
    if ((pad & BOTH) == BOTH && !hidden) {
        hidden = 1;
        memset(past, 0, sizeof past);
        menu_command(MENU_TOGGLE);
    }
    if (hidden) {
        if (!(pad & BOTH))
            hidden = 0;
        return (uint16_t)(pad & ~BOTH);
    }
    late = past[at];
    past[at] = (uint16_t)(pad & BOTH);
    at = (at + 1) % LATE;
    return (uint16_t)((pad & ~BOTH) | late);
}

/* What a controller's button is called in the menu's line about the controls. */
static const char *pad_name(SDL_GamepadButton button)
{
    static const char *const face[] = { NULL, "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle" };
    static const char *const face_es[] = { NULL, "A", "B", "X", "Y", "Cruz", "C\xC3\xADrculo", "Cuadrado", "Tri\xC3\xA1ngulo" };
    int label = gamepad ? (int)SDL_GetGamepadButtonLabel(gamepad, button) : 0;
    if (label >= 1 && label <= 8)
        return spanish() ? face_es[label] : face[label];
    switch (button) {
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return spanish() ? "Arriba" : "Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return spanish() ? "Abajo" : "Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return spanish() ? "Izquierda" : "Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return spanish() ? "Derecha" : "Right";
    default: break;
    }
    return "?";
}

static const char *key_name(SDL_Scancode code)
{
    const char *name = SDL_GetScancodeName(code);
    return name && *name ? name : "?";
}

/* The lines under the entries: whether the game waits, the controls as they are bound (they are changed in the
 * launcher), and how the menu is used. */
static int menu_info(char lines[INFO_LINES][INFO_BYTES])
{
    const int es = spanish();
    int n = 0;
    SDL_snprintf(lines[n++], INFO_BYTES, "%s", menu_online ? (es ? "En l\xC3\xADnea: el juego sigue en marcha" : "Online: the game keeps running")
                                                            : (es ? "Juego en pausa" : "Game paused"));
#ifndef __ANDROID__
    SDL_snprintf(lines[n++], INFO_BYTES, "%s: A=%s B=%s L=%s R=%s", es ? "Teclas" : "Keys", key_name(keys[0]), key_name(keys[1]),
                 key_name(keys[9]), key_name(keys[8]));
    SDL_snprintf(lines[n++], INFO_BYTES, "Start=%s Select=%s", key_name(keys[3]), key_name(keys[2]));
#endif
    if (gamepad) {
        SDL_snprintf(lines[n++], INFO_BYTES, "%s: A=%s B=%s L=%s R=%s", es ? "Mando" : "Pad", pad_name(pad_buttons[0]), pad_name(pad_buttons[1]),
                     pad_name(pad_buttons[9]), pad_name(pad_buttons[8]));
        SDL_snprintf(lines[n++], INFO_BYTES, "Start=%s Select=%s", pad_name(pad_buttons[3]), pad_name(pad_buttons[2]));
        SDL_snprintf(lines[n++], INFO_BYTES, "%s", es ? "Select+Start: men\xC3\xBA. A: elegir. B: volver" : "Select+Start: menu. A: choose. B: back");
    } else {
#ifdef __ANDROID__
        SDL_snprintf(lines[n++], INFO_BYTES, "%s", es ? "Toca una opci\xC3\xB3n para cambiarla" : "Touch an option to change it");
#else
        SDL_snprintf(lines[n++], INFO_BYTES, "%s", es ? "Flechas: mover y cambiar. Esc: volver" : "Arrows: move and change. Esc: back");
#endif
    }
    return n;
}

/* Draw the menu over the finished picture. It has its own scale: the largest whole number at which the panel
 * fits the window, whatever the picture's mode, so it reads the same over a small classic picture and a wide
 * one. In a window too small for the game's typeface it falls back to the 5 x 7 dots. The look is the port's
 * "Soul" one, as the launcher's: a deep blue-green panel with points of light, a gold frame and jewels. */
static void draw_menu(void)
{
    char info[INFO_LINES][INFO_BYTES], buffer[32];
    int items[ITEM_KINDS], count = menu_items(items), lines = menu_info(info);
    int out_w = 0, out_h = 0, attempt, game = 0, pitch = 0, info_pitch = 0, title = 0, w = 0, h = 0, scale = 1, x, y, i, k, row_y, tall;
    const char *heading = spanish() ? "Opciones" : "Options";

    SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    logical_w = logical_h = 0;
    for (attempt = 0; attempt < 2; attempt++) {
        block_text = attempt;
        game = text_game();
        pitch = game ? 15 : 11;
        info_pitch = game ? 14 : 9;
        title = game ? 21 : 14;
        w = game ? 272 : 244;
        h = 5 + title + count * pitch + 6 + lines * info_pitch + 4;
        scale = out_w / (w + 8) < out_h / (h + 8) ? out_w / (w + 8) : out_h / (h + 8);
        if (scale >= 1 || !game)
            break;
    }
    if (scale < 1)
        scale = 1;
    if (menu_row >= count)
        menu_row = count - 1;
    tall = text_height();
    x = (out_w / scale - w) / 2;
    y = (out_h / scale - h) / 2;
    SDL_SetRenderScale(renderer, (float)scale, (float)scale);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
    SDL_RenderFillRect(renderer, NULL);
    fill((float)x - 3, (float)y - 3, (float)w + 6, (float)h + 6, 96, 58, 16);
    fill((float)x - 2, (float)y - 2, (float)w + 4, (float)h + 4, 232, 168, 56);
    fill((float)x - 1, (float)y - 1, (float)w + 2, (float)h + 2, 255, 226, 138);
    for (i = 0; i < h; i++) {
        int t = i * 255 / (h - 1);
        fill((float)x, (float)(y + i), (float)w, 1, 16 - t * 8 / 255, 50 - t * 26 / 255, 60 - t * 28 / 255);
    }
    for (i = 3; i < h; i += 7)
        for (k = 3 + (i * 5) % 11; k < w; k += 13) {
            int light = (k * 31 + i * 17) % 5;
            fill((float)(x + k), (float)(y + i), 1, 1, 30 + light * 8, 74 + light * 9, 84 + light * 9);
        }
    jewel(x - 2, y - 2, 3, 232, 168, 56); jewel(x + w + 1, y - 2, 3, 232, 168, 56);
    jewel(x - 2, y + h + 1, 3, 232, 168, 56); jewel(x + w + 1, y + h + 1, 3, 232, 168, 56);
    jewel(x + w / 2, y - 2, 3, 92, 214, 226); jewel(x + w / 2, y + h + 1, 3, 92, 214, 226);

    draw_text(heading, -1, x + (w - text_width(heading, -1)) / 2, y + 5 + (title - 4 - tall) / 2, 255, 214, 110, 255);
    fill((float)x + 8, (float)(y + 5 + title - 3), (float)w - 16, 1, 170, 118, 40);
    row_y = y + 5 + title;
    for (i = 0; i < count; i++) {
        const char *value = item_value(items[i], buffer, sizeof buffer);
        int top = row_y + i * pitch, text_y = top + (pitch - tall) / 2, chosen = i == menu_row, wide, value_x;
        if (chosen) {
            fill((float)x + 4, (float)top, (float)w - 8, (float)pitch, 150, 98, 26);
            fill((float)x + 4, (float)top, (float)w - 8, 1, 232, 168, 56);
        }
        if (items[i] == ITEM_QUIT && menu_quit_armed)
            draw_text(item_label(items[i]), -1, x + 10, text_y, 255, 150, 150, 255);
        else
            draw_text(item_label(items[i]), -1, x + 10, text_y, chosen ? 255 : 226, chosen ? 255 : 236, chosen ? 255 : 230, 255);
        menu_shape.items[i] = items[i];
        menu_shape.middle[i] = menu_shape.reach[i] = 0;
        if (!value)
            continue;
        wide = text_width(value, -1);
        value_x = x + w - 20 - wide;
        draw_text(value, -1, value_x, text_y, 255, 226, 138, 255);
        for (k = 0; k < 4; k++) {       /* the arrows at both sides of the value */
            int shade = chosen ? 255 : 150;
            fill((float)(value_x - 10 + k), (float)(top + pitch / 2 - k), 1, (float)(2 * k + 1), shade, shade * 226 / 255, shade * 138 / 255);
            fill((float)(x + w - 14 + k), (float)(top + pitch / 2 - 3 + k), 1, (float)(7 - 2 * k), shade, shade * 226 / 255, shade * 138 / 255);
        }
        menu_shape.middle[i] = value_x + wide / 2;
        menu_shape.reach[i] = value_x - 22;
    }
    row_y += count * pitch + 6;
    fill((float)x + 8, (float)(row_y - 3), (float)w - 16, 1, 170, 118, 40);
    for (i = 0; i < lines; i++) {
        int warn = i == 0 && menu_online;
        draw_text(info[i], text_fit(info[i], w - 20), x + 10, row_y + i * info_pitch + (info_pitch - tall) / 2 + (game ? 1 : 0),
                  warn ? 255 : 150, warn ? 190 : 196, warn ? 120 : 204, 255);
    }
    menu_shape.scale = scale;
    menu_shape.x = x;
    menu_shape.w = w;
    menu_shape.rows_y = y + 5 + title;
    menu_shape.pitch = pitch;
    menu_shape.count = count;
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    block_text = 0;
}

static void test_report(void)
{
    fprintf(stderr, "menu test: %ld pictures shown for %ld game frames\n", pictures, game_frames);
}

/* SS_TEST_KEYS, see "options menu". */
static void test_keys(void)
{
    static const char *const names[] = { "up", "down", "left", "right", "ok", "back", "esc" };     /* and: min, restore */
    static const char *script;
    static int looked;
    const char *cursor;
    if (!looked) {
        looked = 1;
        script = getenv("SS_TEST_KEYS");
        test_log = script != NULL;
        if (test_log)
            atexit(test_report);
    }
    for (cursor = script; cursor && *cursor; ) {
        char *end, name[32];
        long at = strtol(cursor, &end, 10);
        size_t length;
        unsigned i;
        if (*end != ':')
            break;
        cursor = end + 1;
        length = strcspn(cursor, ",");
        if (at == pictures && length < sizeof name) {
            memcpy(name, cursor, length);
            name[length] = 0;
            for (i = 0; i < sizeof names / sizeof names[0]; i++)
                if (!strcmp(name, names[i]))
                    menu_command((int)i);           /* the order of the MENU_ commands */
            if (!strncmp(name, "pad:", 4))
                test_pad = (uint16_t)strtoul(name + 4, NULL, 16);
            if (!strcmp(name, "reset"))             /* as if the graphics device had been reset */
                make_textures();
            if (!strcmp(name, "min"))               /* the window is minimised, and brought back */
                SDL_MinimizeWindow(window);
            if (!strcmp(name, "restore"))
                SDL_RestoreWindow(window);
            if (!strncmp(name, "click:", 6)) {
                float x = (float)strtol(name + 6, &end, 10), y = *end == ':' ? (float)atol(end + 1) : 0;
                menu_click(x, y, 0);
                menu_click(x, y, 1);
            }
        }
        cursor += length;
        if (*cursor == ',')
            cursor++;
    }
}

/* The buttons the keyboard and the controller hold for the game (bit order above). */
static uint16_t qol_buttons(const bool *state);    /* further down, with the option it belongs to */

static uint16_t read_buttons(void)
{
    const bool *state = SDL_GetKeyboardState(NULL);
    uint16_t held = 0, pad = test_pad;
    int i;
    for (i = 0; i < 10; i++)
        if (state[keys[i]])
            held |= (uint16_t)(1 << i);
    if (gamepad) {
        /* the stick acts as the d-pad */
        int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        for (i = 0; i < 10; i++)
            if (SDL_GetGamepadButton(gamepad, pad_buttons[i]))
                pad |= (uint16_t)(1 << i);
        if (x > 12000) pad |= 1 << 4;
        if (x < -12000) pad |= 1 << 5;
        if (y < -12000) pad |= 1 << 6;
        if (y > 12000) pad |= 1 << 7;
    }
    held |= menu_chord(pad);
    menu_buttons(pad);
    if ((held & 0x30) == 0x30) held &= ~0x30;       /* left+right or up+down cannot happen on the console's pad */
    if ((held & 0xC0) == 0xC0) held &= ~0xC0;
    held |= qol_buttons(state);         /* experimental: the triggers, as two buttons above the console's ten */
    return held;
}

/* Draw the frame in the texture (`width` columns), what goes over it, and the menu. Returns the buttons the
 * touch controls hold. The picture shown is `width` wide except right after the mode changed and while a game
 * waits behind the menu, when the frame at hand has the width of the mode before: its middle is shown then, or
 * all of it in the middle. */
static uint16_t show_picture(int width)
{
    int view = classic ? SS_SCREEN_W : menu_open ? (view_forced ? view_forced : ss_plat_view_width()) : width;
    int shown = width < view ? width : view, w = classic ? view + 2 * border : view, h = classic ? SS_SCREEN_H + 2 * border : SS_SCREEN_H;
    SDL_FRect source = { (float)((width - shown) / 2), 0, (float)shown, SS_SCREEN_H };
    SDL_FRect inside = { (float)((classic ? border : 0) + (view - shown) / 2), (float)(classic ? border : 0), (float)shown, SS_SCREEN_H };
    uint16_t held;

    if (w != logical_w || h != logical_h) {
        SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        logical_w = w;
        logical_h = h;
    }
    if (classic) {
        draw_border();
        SDL_RenderTexture(renderer, texture, &source, &inside);
        draw_overlay(w, h);
        draw_notice(border, border, view);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        /* Enhanced mode: where the picture has nothing to show (beside a menu, past the edge of a map) the
         * renderer leaves the pixels transparent and the port's field shows through. SS_SIDES=0 keeps black. */
        if (view > SS_SCREEN_W && sides_field)
            draw_field(view, SS_SCREEN_H, 0, 0, 0, 0);
        SDL_RenderTexture(renderer, texture, &source, &inside);
        draw_overlay(w, h);
        draw_notice(0, 0, view);
    }
    held = touch_buttons(w);
    if (menu_open)
        draw_menu();
    return held;
}

/* ---- quality of life, experimental: the L and R bars step backwards (core/qol.c) ----
 * Two buttons the console does not have, reported above its ten: the left trigger (LT, L2) moves the L bar one
 * slot backwards and the right trigger (RT, R2) the R bar. On the keyboard they are D and F, or the two SDL
 * scancodes of SS_KEYS_QOL ("7,9"). Only reported while the option is on: SS_QOL_BARS=1 at the start, and
 * ss_qol_set_bars while the game runs. */
static int qol_bars = -1;

int ss_qol_bars(void)
{
    if (qol_bars < 0) {
        const char *text = getenv("SS_QOL_BARS");
        qol_bars = text && text[0] == '1';
    }
    return qol_bars;
}

void ss_qol_set_bars(int on)
{
    qol_bars = on != 0;
}

static uint16_t qol_buttons(const bool *state)
{
    static int scancode[2] = { SDL_SCANCODE_D, SDL_SCANCODE_F }, read, pulled[2];
    static const SDL_GamepadAxis trigger[2] = { SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
    uint16_t held = 0;
    int i;
    if (!read) {
        const char *text = getenv("SS_KEYS_QOL");
        for (i = 0; text && *text && i < 2; i++) {
            char *end;
            long value = strtol(text, &end, 10);
            if (end == text)
                break;
            if (value >= 1 && value < SDL_SCANCODE_COUNT)
                scancode[i] = (int)value;
            text = *end == ',' ? end + 1 : end;
        }
        read = 1;
    }
    if (!ss_qol_bars())
        return 0;
    for (i = 0; i < 2; i++) {
        if (gamepad) {      /* a trigger is an axis, 0 to 32767: pulled past half, let go under a quarter */
            int value = SDL_GetGamepadAxis(gamepad, trigger[i]);
            pulled[i] = value > 16000 || (pulled[i] && value > 8000);
        } else
            pulled[i] = 0;
        if (state[scancode[i]] || pulled[i])
            held |= (uint16_t)(SS_KEY_L_BACK << i);
    }
    return held;
}

/* Show one `width` x 160 ARGB frame, wait until the next 1/59.7275 s, and return the buttons held (bit order above). */
uint16_t ss_plat_present(const uint32_t *pixels, int width)
{
    SDL_Rect update = { 0, 0, width, SS_SCREEN_H };
    uint16_t held = 0;
    uint64_t now, part_from = 0, part_to;
    uint32_t *part = stat_part_us[PART_UPLOAD] ? stat_part_us[PART_UPLOAD] + stat_part_count % STAT_SLOTS : NULL;
    SDL_Event event;
    static int unpaced;                 /* frames in a row the display was to pace and did not */
    static uint64_t ideal_ns;           /* when this game frame was due by the console's clock, however it is paced */
    static int frameskip = -1, test_slow_ms, after_left_out;
    int waiting, late;

    if (!window)
        return 0;
    if (frameskip < 0) {
        /* SS_FRAMESKIP=0: never leave a picture out. SS_TEST_SLOW=ms: every picture takes that much longer, as on
         * a machine too slow for the game (tests). */
        frameskip = !(getenv("SS_FRAMESKIP") && getenv("SS_FRAMESKIP")[0] == '0');
        test_slow_ms = getenv("SS_TEST_SLOW") ? atoi(getenv("SS_TEST_SLOW")) : 0;
    }
    game_frames++;
    stat_game_frames++;
    if (!stat_game_first_ns)
        stat_game_first_ns = SDL_GetTicksNS();
    if (!width) {
        /* a frame without a picture (see runtime.h): everything of a frame but the picture and the wait */
        while (SDL_PollEvent(&event))
            handle_event(&event);
        test_keys();
        held = read_buttons();
        next_frame_ns += 16742706;
        ideal_ns += 16742706;
        now = SDL_GetTicksNS();
        stat_game_last_ns = now;
        stat_left_out++;
        if (ideal_ns > now) {           /* caught up, with time to spare: the rest of this frame's time is waited out */
            SDL_DelayPrecise(ideal_ns - now);
            now = ideal_ns;
        }
        after_left_out = 1;             /* the picture that follows is late already: it waits for nothing */
        late =now - ideal_ns > 16742706 && !menu_open && !overlay_skipping;
        if (menu_open) {
            held = 0;
        } else {
            menu_swallow &= held;
            held &= (uint16_t)~menu_swallow;
        }
        return (uint16_t)(held | (late ? SS_PRESENT_LATE : 0));
    }
    if (test_slow_ms)
        SDL_DelayPrecise((uint64_t)test_slow_ms * 1000000u);
    if (part)
        part_from = SDL_GetTicksNS();
    SDL_UpdateTexture(texture, &update, pixels, width * 4);
    if (part) {
        part_to = SDL_GetTicksNS();
        *part = (uint32_t)((part_to - part_from) / 1000);
    }
    do {                                /* once, or for as long as a game alone waits behind the menu */
        while (SDL_PollEvent(&event))
            handle_event(&event);
        pictures++;
        test_keys();
        held = read_buttons();
        if (classic < 0)
            read_mode();
        menu_online = menu_open && ss_net_active();
        if (part)
            part_from = SDL_GetTicksNS();
        if (texture_remade)             /* the device was reset: the new texture has not seen this frame */
            SDL_UpdateTexture(texture, &update, pixels, width * 4);
        held |= show_picture(width);
        texture_remade = 0;
        if (part) {
            part_to = SDL_GetTicksNS();
            stat_part_us[PART_DRAW][stat_part_count % STAT_SLOTS] = (uint32_t)((part_to - part_from) / 1000);
        }
        {
            /* SS_WINDOW_SHOT=N: save what the window shows at presented frame N in ss_window.bmp. More numbers
             * after commas save further pictures of the same run, each in ss_window_N.bmp. */
            static const char *shots;
            static int looked;
            const char *cursor;
            char name[40], *end;
            if (!looked) {
                looked = 1;
                shots = getenv("SS_WINDOW_SHOT");
            }
            for (cursor = shots; cursor && *cursor; cursor = *end == ',' ? end + 1 : NULL) {
                long shot_at = strtol(cursor, &end, 10);
                if (shot_at && pictures == shot_at) {
                    SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
                    SDL_snprintf(name, sizeof name, cursor == shots ? "ss_window.bmp" : "ss_window_%ld.bmp", shot_at);
                    if (surface) {
                        SDL_SaveBMP(surface, name);
                        SDL_DestroySurface(surface);
                    }
                }
                if (end == cursor)
                    break;
            }
        }
        if (part)
            part_from = SDL_GetTicksNS();
        SDL_RenderPresent(renderer);
        if (part) {
            part_to = SDL_GetTicksNS();
            stat_part_us[PART_PRESENT][stat_part_count % STAT_SLOTS] = (uint32_t)((part_to - part_from) / 1000);
            if (stat_first_frame_ms < 0)
                stat_first_frame_ms = process_age_ms();
        }

        /* the console refreshes at 59.7275 Hz; game speed must not depend on the monitor */
        next_frame_ns += 16742706;
        now = SDL_GetTicksNS();
        if (after_left_out) {
            next_frame_ns = now;                        /* frames went by without a picture to make up time */
            after_left_out = 0;
        } else if (display_paced && !overlay_skipping && now + 2500000 >= next_frame_ns) {
            next_frame_ns = now;                        /* the display's refresh already spaced this frame */
            unpaced = 0;
        } else if (next_frame_ns > now) {
            /* (also when the display was to pace the game and the present call came back well before a frame's
             * time: a minimised or covered window, a driver told to ignore synchronisation, a display that is
             * faster than it said. The game's own clock keeps its speed then, instead of running as fast as
             * pictures can be shown. After half a second of that the display is no longer taken as the pace:
             * the sound goes back to its own speed too. It is decided again when the window comes back or the
             * display changes, see handle_event.) */
            if (display_paced && ++unpaced == 30) {
                display_paced = 0;
                audio_ratio = 1.0f;
                audio_trim = 0;
                if (audio)
                    SDL_SetAudioStreamFrequencyRatio(audio, audio_ratio);
                fprintf(stderr, "video: the display does not space the frames as it said: own pacing\n");
            }
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
        waiting = menu_open && !menu_online;
        if (waiting && ss_plat_idle)
            ss_plat_idle();
    } while (waiting);
    if (part)
        stat_part_count++;

    /* The game gets nothing while the menu is open, and afterwards nothing of what was held in it (the B that
     * closed it, the A that chose "resume") until that button has been let go once. */
    if (menu_open) {
        held = 0;
    } else {
        menu_swallow &= held;
        held &= (uint16_t)~menu_swallow;
    }
    if (test_log) {
        static uint16_t told;
        if (held != told)
            fprintf(stderr, "input: picture %ld, game frame %ld: the game is given %04X\n", pictures, game_frames, held);
        told = held;
    }
    /* How far behind the console's clock this frame ended, whoever paces the game. Ahead of it (a display a little
     * faster than the console) or hopelessly behind (the window was dragged) the clock starts again from now. */
    ideal_ns += 16742706;
    now = SDL_GetTicksNS();
    stat_game_last_ns = now;
    if (ideal_ns > now || now - ideal_ns > 100000000)
        ideal_ns = now;
    late = frameskip && now - ideal_ns > 16742706 && !menu_open && !overlay_skipping;
    return (uint16_t)(held | (late ? SS_PRESENT_LATE : 0));
}
