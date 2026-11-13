/* ShiningSource on the PS Vita: the host program around the game image (never run on a console yet).
 *
 * The Vita runs 32-bit ARM code, so the game and the runtime core are ordinary code of this program, linked in
 * as one object with its own small C library (runtime/guest) and its memory rebased (rebase.c). They reach the
 * console only through the bridge table of runtime/guest/bridge.h, which here is a table of plain functions.
 *
 *   ROM       the user's own dump, any file ending in .gba in ux0:data/ShiningSource/ (nothing of the game is
 *             in the package)
 *   saves     ux0:data/ShiningSource/shiningsource.sav, the cartridge's own format
 *   settings  ux0:data/ShiningSource/settings.txt, optional: lines NAME=VALUE with the port's SS_ variables
 *   log       ux0:data/ShiningSource/log.txt
 *   picture   320 columns of the wide view at 3x = 960 x 480 on the 960 x 544 screen (scenes that keep the
 *             console's composition are 240 columns, centred), through SDL2's renderer
 *   sound     48 kHz stereo through SDL2; the game is paced by what the sound device has taken
 *   buttons   Circle = A, Cross = B, L, R, Start, Select, the D-pad or the left stick
 */
#include "../guest/bridge.h"

#include <SDL2/SDL.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#define DATA "ux0:data/ShiningSource/"
enum { SCREEN_W = 960, SCREEN_H = 544, VIEW_W = 320, MAX_W = 480, ROWS = 160 };

unsigned int _newlib_heap_size_user = 48 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;

void ss_guest_entry(const uint32_t *bridge, uint32_t stack_low, uint32_t stack_high);
extern const char *ss_guest_rom;

static FILE *log_file;
static FILE *files[32];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio;
static int darkness;

#define P(address) ((void *)(uintptr_t)(address))

static const char *data_path(const char *path, char *buffer, size_t size)
{
    if (strchr(path, ':'))
        return path;
    snprintf(buffer, size, DATA "%s", path);
    return buffer;
}

/* ---- settings.txt: what getenv is on a PC ---- */

static char settings[8192];

static int b_getenv(uint32_t name, uint32_t out, uint32_t capacity)
{
    size_t length = strlen(P(name));
    const char *line = settings;
    while (line && *line) {
        const char *end = strpbrk(line, "\r\n");
        size_t size = end ? (size_t)(end - line) : strlen(line);
        if (size > length && line[length] == '=' && memcmp(line, P(name), length) == 0) {
            size -= length + 1;
            if (size + 1 > capacity)
                return -1;
            memcpy(P(out), line + length + 1, size);
            ((char *)P(out))[size] = 0;
            return (int)size;
        }
        line = end ? end + strspn(end, "\r\n") : NULL;
    }
    return -1;
}

/* ---- files ---- */

static uint32_t b_version(void) { return SS_BRIDGE_VERSION; }

static int b_write(int handle, uint32_t data, uint32_t size)
{
    FILE *file = handle == 1 || handle == 2 ? log_file : handle >= 3 && handle < 35 ? files[handle - 3] : NULL;
    int put;
    if (!file)
        return handle == 1 || handle == 2 ? (int)size : -1;
    put = (int)fwrite(P(data), 1, size, file);
    if (file == log_file)
        fflush(file);
    return put;
}

static int b_open(uint32_t path, uint32_t mode)
{
    char full[512];
    int i;
    for (i = 0; i < 32; i++) {
        if (!files[i]) {
            files[i] = fopen(data_path(P(path), full, sizeof full), P(mode));
            return files[i] ? i + 3 : 0;
        }
    }
    return 0;
}

static int b_read(int handle, uint32_t data, uint32_t size)
{
    return handle >= 3 && handle < 35 && files[handle - 3] ? (int)fread(P(data), 1, size, files[handle - 3]) : -1;
}

static int b_close(int handle)
{
    int result = -1;
    if (handle >= 3 && handle < 35 && files[handle - 3]) {
        result = fclose(files[handle - 3]);
        files[handle - 3] = NULL;
    }
    return result;
}

static int b_remove(uint32_t path)
{
    char full[512];
    return remove(data_path(P(path), full, sizeof full));
}

static int b_rename(uint32_t from, uint32_t to)
{
    char a[512], b[512];
    const char *target = data_path(P(to), b, sizeof b);
    remove(target);                     /* the Vita does not replace an existing file */
    return rename(data_path(P(from), a, sizeof a), target);
}

static void b_exit(int code)
{
    if (log_file) {
        fprintf(log_file, "exit %d\n", code);
        fclose(log_file);
    }
    SDL_Quit();
    sceKernelExitProcess(code);
}

/* ---- picture, buttons, sound ---- */

static int b_video_init(uint32_t title)
{
    SDL_AudioSpec want;
    (void)title;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
        return -1;
    window = SDL_CreateWindow("ShiningSource", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, MAX_W, ROWS) : NULL;
    if (!texture)
        return -1;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    memset(&want, 0, sizeof want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (audio)
        SDL_PauseAudioDevice(audio, 0);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    return 0;
}

static int b_view_width(void) { return VIEW_W; }

static uint32_t buttons(void)
{
    SceCtrlData pad;
    uint32_t held = 0;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_CIRCLE) held |= 0x001;
    if (pad.buttons & SCE_CTRL_CROSS) held |= 0x002;
    if (pad.buttons & SCE_CTRL_SELECT) held |= 0x004;
    if (pad.buttons & SCE_CTRL_START) held |= 0x008;
    if ((pad.buttons & SCE_CTRL_RIGHT) || pad.lx > 192) held |= 0x010;
    if ((pad.buttons & SCE_CTRL_LEFT) || pad.lx < 64) held |= 0x020;
    if ((pad.buttons & SCE_CTRL_UP) || pad.ly < 64) held |= 0x040;
    if ((pad.buttons & SCE_CTRL_DOWN) || pad.ly > 192) held |= 0x080;
    if (pad.buttons & SCE_CTRL_RTRIGGER) held |= 0x100;
    if (pad.buttons & SCE_CTRL_LTRIGGER) held |= 0x200;
    return held;
}

static void b_pump(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_QUIT)
            b_exit(0);
}

/* One frame: `width` x 160 ARGB pixels (width 0: nothing new to show). Returns the held buttons. */
static uint32_t b_present(uint32_t pixels, int width)
{
    b_pump();
    if (width > 0 && width <= MAX_W && texture) {
        int scale = SCREEN_W / width < SCREEN_H / ROWS ? SCREEN_W / width : SCREEN_H / ROWS;
        SDL_Rect source = { 0, 0, width, ROWS }, target;
        target.w = width * scale;
        target.h = ROWS * scale;
        target.x = (SCREEN_W - target.w) / 2;
        target.y = (SCREEN_H - target.h) / 2;
        SDL_UpdateTexture(texture, &source, P(pixels), width * 4);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, &source, &target);
        if (darkness > 0) {             /* the fade of "skip scene" */
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(darkness > 255 ? 255 : darkness));
            SDL_RenderFillRect(renderer, &target);
        }
        SDL_RenderPresent(renderer);
    }
    /* pace: the game makes 800 sample frames of sound a frame; wait while the device still holds four frames */
    while (audio && SDL_GetQueuedAudioSize(audio) > 4 * 804 * 4)
        SDL_Delay(1);
    return buttons();
}

static void b_audio(uint32_t samples, int frames)
{
    if (audio && frames > 0)
        SDL_QueueAudio(audio, P(samples), (Uint32)frames * 4);
}

/* ---- the rest of the table ---- */

static uint32_t b_stack_alloc(uint32_t size) { return (uint32_t)(uintptr_t)malloc(size); }
static void b_stack_free(uint32_t base, uint32_t size) { (void)size; free(P(base)); }
static void b_stack_bounds(uint32_t low, uint32_t high) { (void)low; (void)high; }
static double b_pow(double a, double b) { return pow(a, b); }
static double b_sin(double a) { return sin(a); }
static double b_cos(double a) { return cos(a); }
static double b_fmod(double a, double b) { return fmod(a, b); }
static int b_net_open(uint32_t host, int port) { (void)host; (void)port; return -1; }     /* no online play here yet */
static int b_net_send(uint32_t data, int size) { (void)data; (void)size; return -1; }
static int b_net_recv(uint32_t data, int capacity) { (void)data; (void)capacity; return 0; }
static void b_net_wait(int milliseconds) { SDL_Delay((Uint32)milliseconds); }
static void b_net_farewell(uint32_t data, int size) { (void)data; (void)size; }
static uint32_t b_ticks(void) { return SDL_GetTicks(); }
static void b_status(uint32_t text) { (void)text; }
static void b_overlay(int dark, uint32_t text) { (void)text; darkness = dark; }
static void b_achievements(int run_flags) { (void)run_flags; }
static void b_option_hook(uint32_t function) { (void)function; }

static uint32_t table[SS_B_COUNT];
#define SET(index, function) table[index] = (uint32_t)(uintptr_t)(function)

static int find_rom(char *path, size_t size)
{
    DIR *folder = opendir(DATA);
    struct dirent *entry;
    int found = 0;
    if (!folder)
        return 0;
    while (!found && (entry = readdir(folder)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length > 4 && strcasecmp(entry->d_name + length - 4, ".gba") == 0) {
            snprintf(path, size, DATA "%s", entry->d_name);
            found = 1;
        }
    }
    closedir(folder);
    return found;
}

int main(void)
{
    static char rom[512];
    FILE *file;
    char *stack;

    log_file = fopen(DATA "log.txt", "w");
    file = fopen(DATA "settings.txt", "r");
    if (file) {
        settings[fread(settings, 1, sizeof settings - 1, file)] = 0;
        fclose(file);
    }
    if (!find_rom(rom, sizeof rom)) {
        /* no folder or no ROM: say so in the log and on the screen for a while */
        if (log_file) {
            fprintf(log_file, "no .gba file in " DATA ": copy your own Shining Soul II dump there\n");
            fclose(log_file);
        }
        if (b_video_init(0) == 0) {
            int i;
            for (i = 0; i < 300; i++) {         /* a dark red screen: the log says why */
                SDL_SetRenderDrawColor(renderer, 96, 0, 0, 255);
                SDL_RenderClear(renderer);
                SDL_RenderPresent(renderer);
            }
        }
        sceKernelExitProcess(1);
        return 1;
    }
    SET(SS_B_VERSION, b_version); SET(SS_B_WRITE, b_write); SET(SS_B_OPEN, b_open); SET(SS_B_READ, b_read);
    SET(SS_B_CLOSE, b_close); SET(SS_B_REMOVE, b_remove); SET(SS_B_RENAME, b_rename); SET(SS_B_GETENV, b_getenv);
    SET(SS_B_EXIT, b_exit); SET(SS_B_VIDEO_INIT, b_video_init); SET(SS_B_PRESENT, b_present); SET(SS_B_AUDIO, b_audio);
    SET(SS_B_STACK_ALLOC, b_stack_alloc); SET(SS_B_STACK_FREE, b_stack_free); SET(SS_B_STACK_BOUNDS, b_stack_bounds);
    SET(SS_B_POW, b_pow); SET(SS_B_SIN, b_sin); SET(SS_B_COS, b_cos); SET(SS_B_FMOD, b_fmod);
    SET(SS_B_VIEW_WIDTH, b_view_width); SET(SS_B_PUMP, b_pump); SET(SS_B_NET_OPEN, b_net_open); SET(SS_B_NET_SEND, b_net_send);
    SET(SS_B_NET_RECV, b_net_recv); SET(SS_B_NET_WAIT, b_net_wait); SET(SS_B_NET_FAREWELL, b_net_farewell);
    SET(SS_B_TICKS, b_ticks); SET(SS_B_STATUS, b_status); SET(SS_B_OVERLAY, b_overlay);
    SET(SS_B_ACHIEVEMENTS, b_achievements); SET(SS_B_OPTION_HOOK, b_option_hook);
    ss_guest_rom = rom;
    stack = (char *)&stack;             /* the game runs on this thread's stack; the bounds are not used here */
    ss_guest_entry(table, (uint32_t)(uintptr_t)stack - 0x100000, (uint32_t)(uintptr_t)stack);
    return 0;
}
