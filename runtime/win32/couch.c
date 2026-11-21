/* Each seat has a Windows process so the cartridge's fixed addresses, globals,
 * save files and multiplayer protocol behave the same as on separate GBAs. */
#include <windows.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "couch.h"
#include "soul_field.h"
#include "wide_art.h"
#include "shiningsource/couch_layout.h"
#include "shiningsource/gamepad.h"
#include "shiningsource/net_endpoint.h"
#include "shiningsource/window_mode.h"

enum { SEATS = SS_COUCH_MAX_PLAYERS, PROFILES = 8, MAX_PADS = 16, AUDIO_FRAMES = 8192 };

struct seat_buffer {
    LONG frame_lock, audio_lock, view, held, admission;
    int picture_width, scene, darkness, audio_at, audio_count;
    unsigned picture_serial;
    char overlay[96];
    uint32_t picture[SS_MAX_VIEW_W * SS_SCREEN_H];
    int16_t audio[AUDIO_FRAMES * 2];
};

struct couch_buffer {
    LONG closing;
    struct seat_buffer seat[SEATS];
};

static struct couch_buffer *shared;
static struct seat_buffer *worker;
static uint64_t worker_due;
static int worker_darkness;
static char worker_overlay[96];

static LONG load_long(LONG *value) { return InterlockedCompareExchange(value, 0, 0); }
static int try_lock(LONG *value) { return InterlockedCompareExchange(value, 1, 0) == 0; }
static void unlock(LONG *value) { InterlockedExchange(value, 0); }

int ss_couch_worker_init(void)
{
    const char *name = getenv("SS_COUCH_MAP");
    if (!name)
        return 0;
    const char *seat = getenv("SS_COUCH_SEAT");
    if (!seat || seat[0] < '0' || seat[0] >= '0' + SEATS || seat[1])
        return -1;
    int index = seat[0] - '0';
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map)
        return -1;
    shared = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *shared);
    CloseHandle(map);
    if (!shared)
        return -1;
    worker = &shared->seat[index];
    return 1;
}

int ss_couch_worker(void) { return worker != NULL; }

void ss_couch_worker_admission(int open)
{
    InterlockedExchange(&worker->admission, open != 0);
}

void ss_couch_worker_pump(void)
{
    if (load_long(&shared->closing))
        exit(0);
}

int ss_couch_worker_width(void)
{
    int width = (int)load_long(&worker->view);
    return width >= SS_SCREEN_W && width <= SS_MAX_VIEW_W ? width : SS_SCREEN_W;
}

void ss_couch_worker_overlay(int darkness, const char *text)
{
    worker_darkness = SDL_clamp(darkness, 0, 255);
    SDL_strlcpy(worker_overlay, text ? text : "", sizeof worker_overlay);
}

uint16_t ss_couch_worker_present(const uint32_t *pixels, int width)
{
    ss_couch_worker_pump();
    int scene = width & SS_PRESENT_WIDE_MASK;
    width &= ~(SS_PRESENT_HUD | SS_PRESENT_WIDE_MASK | SS_PRESENT_LANG_MASK);
    if (width >= SS_SCREEN_W && width <= SS_MAX_VIEW_W && try_lock(&worker->frame_lock)) {
        worker->picture_width = width;
        worker->scene = scene;
        worker->darkness = worker_darkness;
        memcpy(worker->overlay, worker_overlay, sizeof worker->overlay);
        memcpy(worker->picture, pixels, (size_t)width * SS_SCREEN_H * sizeof *pixels);
        ++worker->picture_serial;
        unlock(&worker->frame_lock);
    }
    const uint64_t interval = 16742706;    /* the GBA's 59.7275 Hz */
    uint64_t now = SDL_GetTicksNS();
    if (!worker_due || now > worker_due + 4 * interval)
        worker_due = now;
    if (worker_due > now)
        SDL_DelayPrecise(worker_due - now);
    worker_due += interval;
    return (uint16_t)load_long(&worker->held);
}

void ss_couch_worker_audio(const int16_t *samples, int frames)
{
    if (frames <= 0 || !try_lock(&worker->audio_lock))
        return;
    for (int i = 0; i < frames; ++i) {
        int at = (worker->audio_at + worker->audio_count) % AUDIO_FRAMES;
        worker->audio[at * 2] = samples[i * 2];
        worker->audio[at * 2 + 1] = samples[i * 2 + 1];
        if (worker->audio_count == AUDIO_FRAMES)
            worker->audio_at = (worker->audio_at + 1) % AUDIO_FRAMES;
        else
            ++worker->audio_count;
    }
    unlock(&worker->audio_lock);
}

struct pad {
    SDL_Gamepad *device;
    SDL_JoystickID id;
    int seat, buttons[10];
    bool gamecube;
    uint16_t stick;
    struct ss_couch_select select;
};

struct seat {
    struct pad *pad;
    HANDLE process, profile_lock;
    SDL_Texture *texture;
    SDL_AudioStream *audio;
    unsigned serial;
    int profile, confirmed, width, darkness;
    SDL_GUID controller_guid;
    bool controller_assigned;
    char overlay[96];
};

struct lounge {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *background;
    HANDLE mapping, job, relay;
    wchar_t exe[MAX_PATH], base[MAX_PATH];
    const wchar_t *rom;
    char map_name[80], endpoint[SS_NET_HOST_CAPACITY + 8], room[48];
    struct seat seat[SEATS];
    struct pad pads[MAX_PADS];
    int players, running, active, joining, enhanced, online, online_available, wide_art, volume, spanish;
    int background_w, background_h;
    const char *error;
    uint64_t notice_until;
};

static int spawn(struct lounge *room, const wchar_t *exe, wchar_t *args, const wchar_t *cwd,
                 STARTUPINFOW *startup, BOOL inherit, HANDLE *process)
{
    PROCESS_INFORMATION child = {0};
    if (!CreateProcessW(exe, args, NULL, NULL, inherit, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, cwd, startup, &child))
        return 0;
    if (!AssignProcessToJobObject(room->job, child.hProcess) || ResumeThread(child.hThread) == (DWORD)-1) {
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
        return 0;
    }
    CloseHandle(child.hThread);
    *process = child.hProcess;
    return 1;
}

static int start_relay(struct lounge *room)
{
    HANDLE read_pipe = NULL, write_pipe = NULL;
    SECURITY_ATTRIBUTES access = {sizeof access, NULL, TRUE};
    STARTUPINFOW startup = {.cb = sizeof startup};
    wchar_t exe[MAX_PATH], args[MAX_PATH + 16];
    char line[256] = {0};
    int port = 0;
    size_t used = 0;
    if (swprintf(exe, MAX_PATH, L"%lsss_relay.exe", room->base) < 0 ||
        swprintf(args, MAX_PATH + 16, L"\"%ls\" 0", exe) < 0 ||
        !CreatePipe(&read_pipe, &write_pipe, &access, 0))
        return 0;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = write_pipe;
    startup.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &access, OPEN_EXISTING, 0, NULL);
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    SetEnvironmentVariableA("SS_RELAY_LOOPBACK", "1");
    int started = spawn(room, exe, args, room->base, &startup, TRUE, &room->relay);
    SetEnvironmentVariableA("SS_RELAY_LOOPBACK", NULL);
    CloseHandle(write_pipe);
    if (startup.hStdInput != INVALID_HANDLE_VALUE)
        CloseHandle(startup.hStdInput);
    if (started) {
        uint64_t limit = SDL_GetTicks() + 2500;
        while (SDL_GetTicks() < limit && !port) {
            DWORD available = 0, got = 0;
            if (!PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL))
                break;
            if (available && ReadFile(read_pipe, line + used, (DWORD)(sizeof line - used - 1), &got, NULL)) {
                used += got;
                line[used] = 0;
                char *mark = strstr(line, "ss_relay on UDP port ");
                if (mark && strchr(mark, '\n'))
                    sscanf(mark, "ss_relay on UDP port %d", &port);
                if (used == sizeof line - 1)
                    break;
            }
            SDL_Delay(10);
        }
    }
    CloseHandle(read_pipe);
    if (port < 1 || port > 65535)
        return 0;
    SDL_snprintf(room->endpoint, sizeof room->endpoint, "127.0.0.1:%d", port);
    return 1;
}

static int profile_directory(struct lounge *room, int profile, wchar_t *path, int mkdir)
{
    wchar_t parent[MAX_PATH];
    if (swprintf(parent, MAX_PATH, L"%lscouch", room->base) < 0 ||
        swprintf(path, MAX_PATH, L"%ls\\Profile %d", parent, profile + 1) < 0)
        return 0;
    if (mkdir) {
        if (!CreateDirectoryW(parent, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return 0;
        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return 0;
    }
    return 1;
}

static int profile_saved(struct lounge *room, int profile)
{
    wchar_t path[MAX_PATH], save[MAX_PATH];
    if (!profile_directory(room, profile, path, 0))
        return 0;
    const wchar_t *files[] = { L"shiningsource.sav", L"shiningsource.sav.bak" };
    for (unsigned i = 0; i < SDL_arraysize(files); ++i)
        if (swprintf(save, MAX_PATH, L"%ls\\%ls", path, files[i]) >= 0 &&
            GetFileAttributesW(save) != INVALID_FILE_ATTRIBUTES)
            return 1;
    return 0;
}

static int start_seat(struct lounge *room, int index)
{
    struct seat *seat = &room->seat[index];
    wchar_t path[MAX_PATH], lock[MAX_PATH], language[MAX_PATH], solo_language[MAX_PATH], args[MAX_PATH + 4];
    char index_text[8];
    STARTUPINFOW startup = {.cb = sizeof startup};
    if (!profile_directory(room, seat->profile, path, 1) ||
        swprintf(lock, MAX_PATH, L"%ls\\session.lock", path) < 0 ||
        swprintf(args, MAX_PATH + 4, L"\"%ls\"", room->exe) < 0)
        return 0;
    seat->profile_lock = CreateFileW(lock, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (seat->profile_lock == INVALID_HANDLE_VALUE) {
        seat->profile_lock = NULL;
        return 0;
    }
    /* A new profile starts in the same language as the main installation, if it has chosen one. */
    if (swprintf(solo_language, MAX_PATH, L"%lsshiningsource-language.txt", room->base) >= 0 &&
        swprintf(language, MAX_PATH, L"%ls\\shiningsource-language.txt", path) >= 0)
        CopyFileW(solo_language, language, TRUE);
    SDL_snprintf(index_text, sizeof index_text, "%d", index);
    SetEnvironmentVariableW(L"SS_LAUNCHER_ROM", room->rom);
    SetEnvironmentVariableA("SS_COUCH_MAP", room->map_name);
    SetEnvironmentVariableA("SS_COUCH_SEAT", index_text);
    SetEnvironmentVariableA("SS_NET", room->endpoint);
    SetEnvironmentVariableA("SS_NET_ROOM", room->online ? NULL : room->room);
    SetEnvironmentVariableA("SS_NET_DEFERRED", room->online ? "1" : "0");
    SetEnvironmentVariableA("SS_NET_DELAY", "0");
    SetEnvironmentVariableA("SS_NET_JOIN", NULL);
    SetEnvironmentVariableA("SS_REMOTE_SFX", "0");
    SetEnvironmentVariableA("SS_RA_SUBMIT", "0");
    SetEnvironmentVariableA("SS_CHEATS", NULL);
    SetEnvironmentVariableA("SS_HUD_SCALE", NULL);
    SetEnvironmentVariableA("SS_VIEW", NULL);
    SetEnvironmentVariableA("SS_MODE", room->enhanced ? "enhanced" : "classic");
    return spawn(room, room->exe, args, path, &startup, FALSE, &seat->process);
}

static int profile_taken(struct lounge *room, int player, int profile)
{
    for (int i = 0; i < room->players + room->joining; ++i)
        if (i != player && room->seat[i].profile == profile)
            return 1;
    return 0;
}

static int admission_open(struct lounge *room)
{
    if (room->players == SEATS)
        return 0;
    if (room->active)
        for (int i = 0; i < room->players; ++i)
            if (!load_long(&shared->seat[i].admission))
                return 0;
    return 1;
}

static int disconnected_seat(struct lounge *room, struct pad *pad)
{
    SDL_GUID guid = SDL_GetGamepadGUIDForID(pad->id);
    int replacement = -1;
    for (int i = 0; i < room->players; ++i) {
        struct seat *seat = &room->seat[i];
        if (seat->pad || !seat->controller_assigned)
            continue;
        if (!memcmp(&seat->controller_guid, &guid, sizeof guid))
            return i;
        if (!room->active && i && replacement < 0)
            replacement = i;
    }
    return replacement;
}

static int can_claim(struct lounge *room, struct pad *pad)
{
    return pad->seat < 0 && (disconnected_seat(room, pad) >= 0 || (!room->joining && admission_open(room)));
}

static void attach_pad(struct lounge *room, struct pad *pad, int index)
{
    struct seat *seat = &room->seat[index];
    seat->controller_guid = SDL_GetGamepadGUIDForID(pad->id);
    seat->controller_assigned = true;
    seat->pad = pad;
    pad->seat = index;
}

static void assign_pad(struct lounge *room, struct pad *pad)
{
    int reconnect = disconnected_seat(room, pad);
    if (reconnect >= 0) {
        attach_pad(room, pad, reconnect);
        return;
    }
    if (room->joining || !admission_open(room))
        return;
    int index = room->players;
    int profile = 0;
    while (profile_taken(room, index, profile))
        ++profile;
    room->seat[index].profile = profile;
    if (room->active)
        room->joining = 1;
    else
        ++room->players;
    attach_pad(room, pad, index);
}

static void read_bindings(const char *value, int out[10], int minimum, int limit)
{
    int mapped[10];
    for (int i = 0; i < 10; ++i) {
        if (!value)
            return;
        char *next;
        long binding = strtol(value, &next, 10);
        if (next == value || binding < minimum || binding >= limit || *next != (i == 9 ? '\0' : ','))
            return;
        mapped[i] = (int)binding;
        value = next + 1;
    }
    memcpy(out, mapped, sizeof mapped);
}

static struct pad *load_pad(struct lounge *room, SDL_JoystickID id)
{
    struct pad *slot = NULL;
    for (int i = 0; i < MAX_PADS; ++i) {
        if (room->pads[i].device && room->pads[i].id == id)
            return &room->pads[i];
        if (!room->pads[i].device && !slot)
            slot = &room->pads[i];
    }
    if (!slot || !(slot->device = SDL_OpenGamepad(id)))
        return NULL;
    SDL_Gamepad *device = slot->device;
    enum ss_pad_profile profile = ss_pad_profile_from_setting(getenv("SS_PAD_PROFILE"));
    *slot = (struct pad){ .device = device, .id = id, .seat = -1 };
    slot->gamecube = ss_pad_uses_gamecube(device, profile);
    ss_pad_defaults(device, profile, slot->buttons);
    read_bindings(getenv(slot->gamecube ? "SS_PAD_GAMECUBE" : "SS_PAD"), slot->buttons, 0, SDL_GAMEPAD_BUTTON_COUNT);
    return slot;
}

static void select_profile(struct lounge *room, int index, int step)
{
    struct seat *seat = &room->seat[index];
    if (!seat->confirmed) {
        do {
            seat->profile = (seat->profile + step + PROFILES) % PROFILES;
        } while (profile_taken(room, index, seat->profile));
    }
}

static const char *loc(struct lounge *room, const char *en, const char *es) { return room->spanish ? es : en; }

static void start_audio(struct lounge *room, int first)
{
    const SDL_AudioSpec spec = { .format = SDL_AUDIO_S16, .channels = 2, .freq = 48000 };
    for (int i = first; i < room->players; ++i) {
        SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        room->seat[i].audio = audio;
        if (audio)
            SDL_ResumeAudioStreamDevice(audio);
    }
    for (int i = 0; i < room->players; ++i)
        if (room->seat[i].audio)
            SDL_SetAudioStreamGain(room->seat[i].audio, room->volume / (100.0f * room->players));
}

static void cancel_join(struct lounge *room)
{
    struct seat *seat = &room->seat[room->players];
    if (seat->pad)
        seat->pad->seat = -1;
    if (seat->profile_lock)
        CloseHandle(seat->profile_lock);
    *seat = (struct seat){0};
    room->joining = 0;
}

static void accept_join(struct lounge *room)
{
    if (!admission_open(room) || !start_seat(room, room->players)) {
        cancel_join(room);
        room->notice_until = SDL_GetTicks() + 5000;
        return;
    }
    room->joining = 0;
    ++room->players;
    if (SDL_WasInit(SDL_INIT_AUDIO))
        start_audio(room, room->players - 1);
}

static void start_game(struct lounge *room)
{
    if (room->players < 2)
        return;
    for (int i = 0; i < room->players; ++i)
        if (!room->seat[i].confirmed || (i && !room->seat[i].pad))
            return;
    room->active = 1;
    if (!room->online && !start_relay(room)) {
        room->error = loc(room, "The local link server could not start.", "No se pudo iniciar el servidor local.");
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        start_audio(room, 0);
    for (int i = 0; i < room->players; ++i) {
        if (!start_seat(room, i)) {
            room->error = loc(room, "A player could not start. Close other games using these profiles.", "No se pudo iniciar un jugador. Cierra las otras sesiones de estos perfiles.");
            InterlockedExchange(&shared->closing, 1);
            return;
        }
    }
}

static void ready_profile(struct lounge *room, int index)
{
    if (room->seat[index].confirmed) {
        if (!index)
            start_game(room);
    } else
        room->seat[index].confirmed = 1;
}

static void pad_event(struct lounge *room, SDL_Event *event)
{
    SDL_JoystickID id = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? event->gbutton.which : event->gdevice.which;
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        load_pad(room, id);
        return;
    }
    for (int i = 0; i < MAX_PADS; ++i) {
        struct pad *pad = &room->pads[i];
        if (!pad->device || pad->id != id)
            continue;
        if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (room->joining && pad->seat == room->players)
                cancel_join(room);
            if (pad->seat >= 0)
                room->seat[pad->seat].pad = NULL;
            SDL_CloseGamepad(pad->device);
            *pad = (struct pad){0};
            return;
        }
        int button = event->gbutton.button;
        if (button == pad->buttons[2] && can_claim(room, pad) && ss_couch_select_press(&pad->select, SDL_GetTicks()))
            assign_pad(room, pad);
        if (room->active && (!room->joining || pad->seat != room->players))
            return;
        if (button == pad->buttons[3] && pad->seat < 0 && !room->seat[0].controller_assigned) {
            attach_pad(room, pad, 0);
            return;
        }
        if (pad->seat < 0)
            return;
        if (!pad->seat && room->online_available && button == SDL_GAMEPAD_BUTTON_NORTH)
            room->online = !room->online;
        else if (!pad->seat && button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
            room->enhanced = !room->enhanced;
        else if (button == pad->buttons[5])
            select_profile(room, pad->seat, -1);
        else if (button == pad->buttons[4])
            select_profile(room, pad->seat, 1);
        else if (button == pad->buttons[0] || button == pad->buttons[3]) {
            if (room->joining)
                accept_join(room);
            else
                ready_profile(room, pad->seat);
        } else if (button == pad->buttons[1]) {
            if (room->joining)
                cancel_join(room);
            else
                room->seat[pad->seat].confirmed = 0;
        }
        return;
    }
}

static void keyboard_event(struct lounge *room, SDL_Scancode code)
{
    if (code == SDL_SCANCODE_ESCAPE && room->joining) {
        cancel_join(room);
        return;
    }
    if (code == SDL_SCANCODE_ESCAPE && (!room->active || room->error)) {
        room->running = 0;
        return;
    }
    if (code == SDL_SCANCODE_F11) {
        SDL_SetWindowFullscreen(room->window, !(SDL_GetWindowFlags(room->window) & SDL_WINDOW_FULLSCREEN));
        return;
    }
    if (room->active)
        return;
    if (code == SDL_SCANCODE_TAB && room->online_available)
        room->online = !room->online;
    else if (code == SDL_SCANCODE_M)
        room->enhanced = !room->enhanced;
    else if (code == SDL_SCANCODE_LEFT || code == SDL_SCANCODE_RIGHT)
        select_profile(room, 0, code == SDL_SCANCODE_RIGHT ? 1 : -1);
    else if (code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER)
        ready_profile(room, 0);
    else if (code == SDL_SCANCODE_BACKSPACE)
        room->seat[0].confirmed = 0;
}

static uint16_t seat_input(struct seat *seat, const bool *keyboard, const int bindings[10])
{
    uint16_t held = 0;
    for (int i = 0; i < 10; ++i)
        if ((keyboard && keyboard[bindings[i]]) ||
            (seat->pad && ss_pad_button_down(seat->pad->device, seat->pad->buttons[i], seat->pad->gamecube)))
            held |= (uint16_t)(1u << i);
    if (seat->pad) {
        seat->pad->stick = ss_pad_stick(seat->pad->device, seat->pad->gamecube, seat->pad->stick);
        held |= seat->pad->stick;
    }
    return held;
}

static void paint_text(struct lounge *room, float x, float y, const char *text, int gold)
{
    SDL_SetRenderDrawColor(room->renderer, gold ? 237 : 217, gold ? 198 : 230, gold ? 112 : 232, 255);
    SDL_RenderDebugText(room->renderer, x, y, text);
}

static void paint_box(struct lounge *room, const SDL_FRect *box, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawColor(room->renderer, r, g, b, a);
    SDL_RenderFillRect(room->renderer, box);
}

static void centered(struct lounge *room, int w, float y, const char *text, int gold)
{
    paint_text(room, (w - (float)strlen(text) * 8) / 2, y, text, gold);
}

static void draw_background(struct lounge *room, int w, int h)
{
    /* Scale the grain with the split while sizing the texture to the current output. */
    int scale = room->players > 2 ? 2 : 3;
    int cols = (w + scale - 1) / scale, rows = (h + scale - 1) / scale;
    if (!room->background || cols != room->background_w || rows != room->background_h) {
        SDL_DestroyTexture(room->background);
        room->background = NULL;
        uint32_t *pixels = malloc((size_t)cols * rows * sizeof *pixels);
        if (pixels) {
            ss_soul_field_pixels(pixels, cols, rows, 0, 0, 0, 0);
            room->background = SDL_CreateTexture(room->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                                 cols, rows);
            if (room->background && !SDL_UpdateTexture(room->background, NULL, pixels, cols * sizeof *pixels)) {
                SDL_DestroyTexture(room->background);
                room->background = NULL;
            }
            free(pixels);
        }
        if (room->background) {
            room->background_w = cols;
            room->background_h = rows;
            SDL_SetTextureScaleMode(room->background, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureColorMod(room->background, 155, 180, 185);
        }
    }
    if (room->background) {
        SDL_FRect dst = {0, 0, (float)(cols * scale), (float)(rows * scale)};
        SDL_RenderTexture(room->renderer, room->background, NULL, &dst);
    }
}

static int waiting_pad(struct lounge *room)
{
    for (int i = 0; i < MAX_PADS; ++i)
        if (room->pads[i].device && can_claim(room, &room->pads[i]))
            return 1;
    return 0;
}

static void breathe_hint(struct lounge *room, int width, float y, const char *text)
{
    unsigned phase = (unsigned)(SDL_GetTicks() % 2000);
    unsigned rise = phase < 1000 ? phase : 2000 - phase;
    Uint8 alpha = (Uint8)(180 + rise * 75 / 1000);
    SDL_FRect banner = {(float)(width - (int)strlen(text) * 8) / 2 - 8, y - 5, (float)strlen(text) * 8 + 16, 18};
    paint_box(room, &banner, 28, 52, 53, alpha);
    SDL_SetRenderDrawColor(room->renderer, 237, 198, 112, alpha);
    SDL_RenderDebugText(room->renderer, banner.x + 8, y, text);
}

static void draw_picker(struct lounge *room, int width, int height)
{
    int scale = width >= 1100 && height >= 600 ? 2 : 1;
    SDL_SetRenderScale(room->renderer, (float)scale, (float)scale);
    int w = width / scale, h = height / scale;
    int card_w = (w - 64) / 2;
    if (card_w > 250)
        card_w = 250;
    int x = (w - 2 * card_w - 12) / 2, y = (h - 245) / 2;
    const char *title = room->enhanced ? loc(room, "LOCAL MULTIPLAYER / ENHANCED", "MULTIJUGADOR LOCAL / MEJORADO") :
                                          loc(room, "LOCAL MULTIPLAYER / ORIGINAL", "MULTIJUGADOR LOCAL / ORIGINAL");
    centered(room, w, y, title, 1);
    centered(room, w, y + 19, room->online ? loc(room, "Online room", "Sala online") :
                                            loc(room, "Private local room", "Sala privada local"), 1);
    centered(room, w, y + 37, loc(room, "Choose a save profile for each player", "Elige un perfil de guardado para cada jugador"), 0);
    for (int i = 0; i < room->players; ++i) {
        struct seat *seat = &room->seat[i];
        SDL_FRect card = { (float)(x + i % 2 * (card_w + 12)), (float)(y + 58 + i / 2 * 60), (float)card_w, 52 };
        char player[48], profile[48];
        paint_box(room, &card, 22, 45, 48, 255);
        SDL_SetRenderDrawColor(room->renderer, seat->confirmed ? 104 : 150, seat->confirmed ? 159 : 127, 86, 255);
        SDL_RenderRect(room->renderer, &card);
        SDL_snprintf(player, sizeof player, loc(room, "Player %d", "Jugador %d"), i + 1);
        SDL_snprintf(profile, sizeof profile, loc(room, "< Profile %d  %s >", "< Perfil %d  %s >"), seat->profile + 1,
                     profile_saved(room, seat->profile) ? loc(room, "Saved", "Guardado") : loc(room, "Empty", "Sin datos"));
        paint_text(room, card.x + 10, card.y + 8, player, 1);
        paint_text(room, card.x + 10, card.y + 23, profile, 0);
        const char *status = i && !seat->pad ? loc(room, "Controller disconnected", "Mando desconectado") :
                             seat->confirmed ? loc(room, "Confirmed", "Confirmado") :
                             !i && !seat->pad ? loc(room, "Keyboard / Start on pad", "Teclado / Start en el mando") :
                             loc(room, "D-pad and Start", "Cruceta y Start");
        paint_text(room, card.x + 10, card.y + 36, status, 0);
    }
    if (waiting_pad(room))
        breathe_hint(room, w, (float)y + 187, loc(room, "Press Select three times on an unassigned controller.", "Pulsa Select tres veces en un mando sin asignar."));
    centered(room, w, y + 204, loc(room, "Player 1 presses Enter or Start to begin", "El jugador 1 pulsa Enter o Start para empezar"), 0);
    centered(room, w, y + 221, room->online ? loc(room, "In Multiplayer, join the same room on each screen.", "En Multijugador, elige la misma sala en cada pantalla.") :
                                             loc(room, "Use Multiplayer on each screen to enter the local room.", "Elige Multijugador en cada pantalla para entrar a la sala."), 0);
    if (room->online_available)
        centered(room, w, y + 238, loc(room, "Tab / top face button to switch room type", "Tab o Y para cambiar el tipo de sala"), 0);
    centered(room, w, y + 255, loc(room, "M / left shoulder to switch display mode", "M o L para cambiar el modo de pantalla"), 0);
    SDL_SetRenderScale(room->renderer, 1, 1);
}

static void draw_audio(struct seat *seat, struct seat_buffer *buffer)
{
    if (!seat->audio || SDL_GetAudioStreamQueued(seat->audio) > 4800 || !try_lock(&buffer->audio_lock))
        return;
    enum { CHUNK = 2048 };
    int16_t samples[CHUNK * 2];
    int count = buffer->audio_count < CHUNK ? buffer->audio_count : CHUNK;
    for (int i = 0; i < count; ++i) {
        int at = (buffer->audio_at + i) % AUDIO_FRAMES;
        samples[i * 2] = buffer->audio[at * 2];
        samples[i * 2 + 1] = buffer->audio[at * 2 + 1];
    }
    buffer->audio_at = (buffer->audio_at + count) % AUDIO_FRAMES;
    buffer->audio_count -= count;
    unlock(&buffer->audio_lock);
    if (count)
        SDL_PutAudioStreamData(seat->audio, samples, count * 2 * sizeof samples[0]);
}

static void draw_seat(struct lounge *room, int i, const struct ss_couch_rect *area, int allow_ultrawide)
{
    static uint32_t picture[SS_MAX_VIEW_W * SS_SCREEN_H];
    struct seat *seat = &room->seat[i];
    struct seat_buffer *buffer = &shared->seat[i];
    int width = 0, scene = 0;
    unsigned serial = 0;
    if (try_lock(&buffer->frame_lock)) {
        if (seat->serial != buffer->picture_serial && buffer->picture_width) {
            width = buffer->picture_width;
            scene = buffer->scene;
            serial = buffer->picture_serial;
            seat->darkness = buffer->darkness;
            memcpy(seat->overlay, buffer->overlay, sizeof seat->overlay);
            memcpy(picture, buffer->picture, (size_t)width * SS_SCREEN_H * sizeof *picture);
        }
        unlock(&buffer->frame_lock);
    }
    if (width) {
        if (room->enhanced && room->wide_art && scene)
            ss_wide_art_fill(picture, width, scene, allow_ultrawide);
        if (!room->wide_art && scene == SS_PRESENT_WIDE_MARBLE && width <= SS_WIDE_16_9_W) {
            int left = (width - SS_SCREEN_W) / 2, right = width - SS_SCREEN_W - left;
            for (int y = 0; y < SS_SCREEN_H; y++) {
                memset(picture + y * width, 0, (size_t)left * sizeof *picture);
                memset(picture + y * width + left + SS_SCREEN_W, 0, (size_t)right * sizeof *picture);
            }
        }
        if (!seat->texture) {
            seat->texture = SDL_CreateTexture(room->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                              SS_MAX_VIEW_W, SS_SCREEN_H);
            if (seat->texture) {
                SDL_SetTextureScaleMode(seat->texture, SDL_SCALEMODE_NEAREST);
                SDL_SetTextureBlendMode(seat->texture, room->enhanced ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
            }
        }
        if (seat->texture) {
            SDL_Rect src = {0, 0, width, SS_SCREEN_H};
            if (SDL_UpdateTexture(seat->texture, &src, picture, width * sizeof *picture)) {
                seat->width = width;
                seat->serial = serial;
            }
        }
    }
    draw_audio(seat, buffer);
    if (!seat->texture || !area->scale)
        return;
    int view = seat->width < area->view ? seat->width : area->view;
    SDL_FRect src = { (float)(seat->width - view) / 2, 0, (float)view, SS_SCREEN_H };
    SDL_FRect dst = { (float)(area->x + (area->view - view) * area->scale / 2), (float)area->y,
                      (float)(view * area->scale), (float)(SS_SCREEN_H * area->scale) };
    /* Keep the host theme beneath transparent margins during a game fade. */
    Uint8 light = (Uint8)(255 - seat->darkness);
    SDL_SetTextureColorMod(seat->texture, light, light, light);
    SDL_RenderTexture(room->renderer, seat->texture, &src, &dst);
    if (seat->overlay[0])
        paint_text(room, dst.x + 8, dst.y + dst.h - 16, seat->overlay, 1);
    if (i && !seat->pad) {
        const char *label = loc(room, "Controller disconnected", "Mando desconectado");
        SDL_FRect note = {dst.x, dst.y, (float)(strlen(label) * 8 + 10), 16};
        paint_box(room, &note, 15, 33, 35, 235);
        paint_text(room, dst.x + 5, dst.y + 4, label, 1);
    }
}

static void draw_active(struct lounge *room, int width, int height, const int bindings[10])
{
    struct ss_couch_rect rect[SEATS] = {0};
    ss_couch_layout(width, height, room->players, room->enhanced, rect);
    const int allow_ultrawide = (int64_t)width * 10 >= (int64_t)height * 22;
    const bool *keyboard = (SDL_GetWindowFlags(room->window) & SDL_WINDOW_INPUT_FOCUS) ? SDL_GetKeyboardState(NULL) : NULL;
    for (int i = 0; i < room->players; ++i) {
        InterlockedExchange(&shared->seat[i].view, rect[i].view);
        /* Keep local players in place while the new controller is choosing a profile. */
        InterlockedExchange(&shared->seat[i].held, room->joining ? 0 : seat_input(&room->seat[i], i ? NULL : keyboard, bindings));
        draw_seat(room, i, &rect[i], allow_ultrawide);
        if (room->seat[i].process && WaitForSingleObject(room->seat[i].process, 0) == WAIT_OBJECT_0)
            room->error = loc(room, "A player closed. Press Esc to end the local session.", "Falta un jugador. Pulsa Esc para cerrar la partida local.");
    }
    if (room->joining && !admission_open(room)) {
        cancel_join(room);
        room->notice_until = SDL_GetTicks() + 5000;
    }
    if (room->joining) {
        int profile = room->seat[room->players].profile;
        char title[64], detail[48];
        SDL_snprintf(title, sizeof title, loc(room, "Choose a save profile for Player %d", "Elige un perfil de guardado para el jugador %d"), room->players + 1);
        SDL_snprintf(detail, sizeof detail, loc(room, "< Profile %d  %s >", "< Perfil %d  %s >"), profile + 1,
                     profile_saved(room, profile) ? loc(room, "Saved", "Guardado") : loc(room, "Empty", "Sin datos"));
        SDL_FRect picker = { (float)(width - 390) / 2, (float)(height - 80) / 2, 390, 80};
        paint_box(room, &picker, 16, 34, 36, 248);
        SDL_SetRenderDrawColor(room->renderer, 150, 127, 86, 255);
        SDL_RenderRect(room->renderer, &picker);
        centered(room, width, picker.y + 14, title, 1);
        centered(room, width, picker.y + 34, detail, 0);
        centered(room, width, picker.y + 55, loc(room, "D-pad to choose / Start to confirm", "Cruceta para elegir / Start para confirmar"), 0);
    } else if (room->notice_until > SDL_GetTicks()) {
        breathe_hint(room, width, (float)height - 16, loc(room, "The extra player could not join the room.", "El jugador adicional no pudo entrar a la sala."));
    } else if (waiting_pad(room)) {
        const char *hint = admission_open(room) ? loc(room, "Press Select three times to add a player.", "Pulsa Select tres veces para agregar un jugador.") :
                                                 loc(room, "Press Select three times to reconnect your controller.", "Pulsa Select tres veces para reconectar el mando.");
        breathe_hint(room, width, (float)height - 16, hint);
    }
}

static int initialize(struct lounge *room)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    DWORD length = GetModuleFileNameW(NULL, room->exe, SDL_arraysize(room->exe));
    if (!length || length >= SDL_arraysize(room->exe))
        return 0;
    SDL_wcslcpy(room->base, room->exe, SDL_arraysize(room->base));
    wchar_t *end = wcsrchr(room->base, L'\\');
    if (!end)
        return 0;
    end[1] = L'\0';
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    room->job = CreateJobObjectW(NULL, NULL);
    if (!room->job || !SetInformationJobObject(room->job, JobObjectExtendedLimitInformation, &limits, sizeof limits))
        return 0;
    SDL_snprintf(room->map_name, sizeof room->map_name, "Local\\ShiningSourceCouch-%lu-%llu",
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    SDL_snprintf(room->room, sizeof room->room, "couch-%lu-%llu",
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    room->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof *shared, room->map_name);
    if (!room->mapping || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    shared = MapViewOfFile(room->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *shared);
    if (!shared)
        return 0;
    for (int i = 0; i < SEATS; ++i)
        shared->seat[i].view = SS_SCREEN_W;
    ss_pad_enable_gamecube();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        return 0;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    SDL_Rect usable;
    int width = 1920, height = 1080;
    int four_by_three = desktop && desktop->w * 3 == desktop->h * 4;
    if (four_by_three)
        width = 1440;
    if (SDL_GetDisplayUsableBounds(display, &usable)) {
        int ratio_w = four_by_three ? 4 : 16, ratio_h = four_by_three ? 3 : 9;
        width = SDL_min(width, usable.w);
        height = SDL_min(height, usable.h);
        width = SDL_min(width, height * ratio_w / ratio_h);
        height = width * ratio_h / ratio_w;
    }
    if (!SDL_CreateWindowAndRenderer("ShiningSource - Couch Multiplayer", width, height, SDL_WINDOW_RESIZABLE,
                                     &room->window, &room->renderer))
        return 0;
    SDL_SetWindowMinimumSize(room->window, 540, 360);
    SDL_SetRenderDrawBlendMode(room->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(room->renderer, 1);
    const char *mode = getenv("SS_COUCH_WINDOW");
    if (ss_window_mode_from_settings(mode, NULL) == SS_WINDOW_WINDOWED ||
        !SDL_SetWindowFullscreen(room->window, true))
        SDL_MaximizeWindow(room->window);
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i)
        load_pad(room, ids[i]);
    SDL_free(ids);
    return 1;
}

static void cleanup(struct lounge *room)
{
    if (shared)
        InterlockedExchange(&shared->closing, 1);
    for (int i = 0; i < room->players; ++i) {
        struct seat *seat = &room->seat[i];
        if (seat->process)
            WaitForSingleObject(seat->process, 500);
        SDL_DestroyAudioStream(seat->audio);
    }
    if (room->job)
        CloseHandle(room->job);
    for (int i = 0; i < room->players; ++i) {
        if (room->seat[i].process) {
            WaitForSingleObject(room->seat[i].process, 1000);
            CloseHandle(room->seat[i].process);
        }
        if (room->seat[i].profile_lock)
            CloseHandle(room->seat[i].profile_lock);
    }
    if (room->relay)
        CloseHandle(room->relay);
    if (shared)
        UnmapViewOfFile(shared);
    if (room->mapping)
        CloseHandle(room->mapping);
    for (int i = 0; i < MAX_PADS; ++i)
        if (room->pads[i].device)
            SDL_CloseGamepad(room->pads[i].device);
    SDL_Quit();
}

int ss_couch_parent(const wchar_t *rom)
{
    const char *mode = getenv("SS_MODE"), *volume = getenv("SS_VOLUME"), *lang = getenv("SS_LANG");
    const char *endpoint = getenv("SS_NET");
    const char *wide_art = getenv("SS_WIDE_PRO");
    struct lounge room = { .rom = rom, .players = 1, .running = 1, .enhanced = !mode || strcmp(mode, "classic"),
                           .spanish = lang && !strcmp(lang, "es"), .volume = volume ? atoi(volume) : 100,
                           .wide_art = !wide_art || wide_art[0] != '0' };
    int bindings[10] = { SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                         SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
                         SDL_SCANCODE_V, SDL_SCANCODE_C };
    read_bindings(getenv("SS_KEYS"), bindings, 1, SDL_SCANCODE_COUNT);
    if (endpoint && *endpoint && SDL_strlcpy(room.endpoint, endpoint, sizeof room.endpoint) < sizeof room.endpoint)
        room.online_available = 1;
    room.volume = SDL_clamp(room.volume, 0, 100);
    if (!initialize(&room)) {
        MessageBoxA(NULL, loc(&room, "Couch multiplayer could not open.", "No se pudo abrir el multijugador local."), "ShiningSource", MB_ICONERROR);
        cleanup(&room);
        return 1;
    }
    while (room.running) {
        SDL_Event event;
        uint64_t started = SDL_GetTicks();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                room.running = 0;
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
                keyboard_event(&room, event.key.scancode);
            else if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED ||
                     event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                pad_event(&room, &event);
            else if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                SDL_DestroyTexture(room.background);
                room.background = NULL;
                for (int i = 0; i < room.players; ++i) {
                    SDL_DestroyTexture(room.seat[i].texture);
                    room.seat[i].texture = NULL;
                    room.seat[i].serial = 0;
                }
            }
        }
        if (!room.running)
            break;
        int width, height;
        if (!SDL_GetRenderOutputSize(room.renderer, &width, &height) || width < 1 || height < 1) {
            SDL_Delay(10);
            continue;
        }
        SDL_SetRenderDrawColor(room.renderer, 10, 28, 35, 255);
        SDL_RenderClear(room.renderer);
        draw_background(&room, width, height);
        if (room.error) {
            centered(&room, width, (float)height / 2 - 10, room.error, 1);
            centered(&room, width, (float)height / 2 + 8, loc(&room, "Esc to close", "Esc para cerrar"), 0);
        } else if (room.active) {
            draw_active(&room, width, height, bindings);
        } else {
            draw_picker(&room, width, height);
        }
        SDL_RenderPresent(room.renderer);
        uint64_t elapsed = SDL_GetTicks() - started;
        if (elapsed < 8)
            SDL_Delay((Uint32)(8 - elapsed));
    }
    cleanup(&room);
    return 0;
}
