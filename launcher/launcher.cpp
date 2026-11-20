/*
 * ShiningSource launcher.
 *
 * Checks the user's cartridge dump (SHA-1), edits the settings file and starts the game. Dear ImGui on SDL3 and
 * OpenGL, the same combination as the ReKHCoM launcher this one is modelled on.
 *
 *   ShiningSource.exe        this launcher
 *   ss2_win64.exe            the game host (64-bit); ss2_game.elf is the game image it loads
 *   shiningsource.ini        settings, next to the launcher
 *
 * Settings reach the game as environment variables (SS_VIEW, SS_FULLSCREEN, SS_VOLUME, SS_NET...), which is also how the
 * automated tests drive it, so there is one mechanism to keep working. The game's own options menu (Esc while
 * playing, runtime/win32/plat_sdl.c) writes what the player changes there back to shiningsource.ini, with the same
 * keys; two of them, "sides" and "smooth", have no control here and the game reads them from the file by itself.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "achievements/ra_client.h"
#include "rc_api_runtime.h"
#include "../runtime/achievements/record.h"
#include "../runtime/include/shiningsource/ini.h"
#include "../runtime/include/shiningsource/net_endpoint.h"
#include "../runtime/include/shiningsource/runtime.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl3.h"
#include "../runtime/include/shiningsource/window_mode.h"
#include "../runtime/include/shiningsource/gamepad.h"
#include "../runtime/include/shiningsource/language_file.h"
#include "theme.h"
#include "rom_browser.h"
#include "test_pad.h"

/* ---- games ---- */

struct Game {
    const char *id, *title, *region, *sha1, *host;
    size_t rom_size;
    bool available;
};
static const Game games[] = {
    { "ss2", "Shining Soul II", "Europe (AU2P)", "aa1288be9257337abdfe3034898cf6c7aa778fbc", "ss2_win64.exe", 16777216, true },
    { "ss1", "Shining Soul", "", "", "", 0, false },
};

/* ---- language ---- */

static int language;     /* 0 English, 1 Spanish */
static const char *text(const char *english, const char *spanish) { return language ? spanish : english; }

/* ---- SHA-1 of a file ---- */

static std::string sha1_of(FILE *file, size_t expected_size)
{
    if (!file)
        return "";
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    std::vector<uint8_t> data;
    uint8_t buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, file)) > 0) {
        if (expected_size && data.size() + got > expected_size) {
            fclose(file);
            return "";
        }
        data.insert(data.end(), buffer, buffer + got);
    }
    bool read_ok = !ferror(file);
    fclose(file);
    if (!read_ok || (expected_size && data.size() != expected_size))
        return "";
    uint64_t bits = (uint64_t)data.size() * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56)
        data.push_back(0);
    for (int i = 7; i >= 0; i--)
        data.push_back((uint8_t)(bits >> (i * 8)));
    for (size_t block = 0; block < data.size(); block += 64) {
        uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)data[block + i * 4] << 24 | (uint32_t)data[block + i * 4 + 1] << 16 | (uint32_t)data[block + i * 4 + 2] << 8 | data[block + i * 4 + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = x << 1 | x >> 31;
        }
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char out[41];
    snprintf(out, sizeof out, "%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
    return out;
}

/* ---- settings ---- */

static std::string folder;                  /* where the launcher is */
static std::map<std::string, std::string> settings;

static std::string setting(const char *key, const char *fallback = "")
{
    auto found = settings.find(key);
    return found == settings.end() ? fallback : found->second;
}

static void load_settings()
{
    FILE *file = fopen((folder + "shiningsource.ini").c_str(), "r");
    char line[2048];
    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        char *equals = strchr(line, '=');
        if (!equals)
            continue;
        std::string key(line, (size_t)(ss_ini_key_end(line, equals) - line));
        std::string value = ss_ini_value(equals);
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
            value.pop_back();
        settings[key] = value;
    }
    fclose(file);
}

static void save_settings()
{
    FILE *file = fopen((folder + "shiningsource.ini").c_str(), "w");
    if (!file)
        return;
    for (const auto &entry : settings)
        fprintf(file, "%s=%s\n", entry.first.c_str(), entry.second.c_str());
    fclose(file);
}

/* ---- ROM ---- */

static std::string rom_status;      /* "", "ok", "wrong", "missing" */

static void check_rom(const Game &game)
{
    std::string path = setting((std::string("rom_") + game.id).c_str());
    if (path.empty()) { rom_status = ""; return; }
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) { rom_status = "missing"; return; }
    rom_status = sha1_of(file, game.rom_size) == game.sha1 ? "ok" : "wrong";
}

/* A ROM beside the program is recognised without asking: when no ROM is set for the game, or the file that was
 * set is gone, the .gba files in the launcher's folder and in the one above it are checked the same way as a
 * chosen one (SHA-1), and the first that is the supported version is taken. Choosing by hand stays as it was. */
static void find_rom_beside(const Game &game)
{
    std::string key = std::string("rom_") + game.id, current = setting(key.c_str());
    if (!current.empty() && GetFileAttributesA(current.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    size_t cut = folder.size() > 1 ? folder.find_last_of("\\/", folder.size() - 2) : std::string::npos;
    std::string places[2] = { folder, cut == std::string::npos ? "" : folder.substr(0, cut + 1) };
    for (const std::string &place : places) {
        WIN32_FIND_DATAA found;
        HANDLE search = place.empty() ? INVALID_HANDLE_VALUE : FindFirstFileA((place + "*.gba").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
            continue;
        do {
            /* a cartridge of this console is at most 32 MiB: nothing larger is read */
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.nFileSizeHigh || found.nFileSizeLow > 32u * 1024 * 1024)
                continue;
            if (sha1_of(fopen((place + found.cFileName).c_str(), "rb"), game.rom_size) == game.sha1) {
                settings[key] = place + found.cFileName;
                save_settings();
                FindClose(search);
                return;
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
}

/* ---- the game's own picture, made from the user's ROM ----
 * The program carries no artwork of the game. Once a ROM is chosen the launcher runs the game itself without a
 * window up to its title screen, lets it write that frame, and shows it. The file stays in the folder "cache"
 * next to the launcher: it is the user's own copy, made on their machine, and is not part of any release. */
static GLuint title_texture;
static std::string title_for;       /* the ROM path the texture was made from */

static void load_title()
{
    std::string path = folder + "cache\\ss_frame.raw";
    FILE *file = fopen(path.c_str(), "rb");
    static uint32_t pixels[240 * 160];
    if (!file)
        return;
    size_t got = fread(pixels, 4, 240 * 160, file);
    fclose(file);
    if (got != 240 * 160)
        return;
    for (uint32_t &pixel : pixels)       /* ARGB as the game wrote it, to the RGBA bytes the texture wants */
        pixel = 0xFF000000u | (pixel & 0xFF00u) | ((pixel >> 16) & 0xFFu) | ((pixel & 0xFFu) << 16);
    if (!title_texture)
        glGenTextures(1, &title_texture);
    glBindTexture(GL_TEXTURE_2D, title_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 240, 160, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void make_title(const Game &game)
{
    std::string rom = setting((std::string("rom_") + game.id).c_str());
    if (rom.empty() || rom == title_for)
        return;
    title_for = rom;
    std::string cache = folder + "cache", mark = cache + "\\made_from.txt", host = folder + game.host;
    CreateDirectoryA(cache.c_str(), nullptr);
    char previous[1024] = "";
    if (FILE *file = fopen(mark.c_str(), "r")) {
        fgets(previous, sizeof previous, file);
        fclose(file);
    }
    if (rom != previous) {
        /* the title screen is up about twelve seconds of game time after power-on; without a window that is an instant */
        std::string command = "\"" + host + "\" \"" + rom + "\"";
        STARTUPINFOA startup = {};
        PROCESS_INFORMATION process = {};
        startup.cb = sizeof startup;
        SetEnvironmentVariableA("SS_FRAMES", "780");
        SetEnvironmentVariableA("SS_DUMP", "760");
        SetEnvironmentVariableA("SS_INPUT", "420:0001,430:0000");
        SetEnvironmentVariableA("SS_SKIP", "0");
        SetEnvironmentVariableA("SS_NET", nullptr);
        SetEnvironmentVariableA("SS_RA_DIR", nullptr);      /* not a game anyone plays: no achievements, no cheats */
        SetEnvironmentVariableA("SS_CHEATS", nullptr);
        if (CreateProcessA(host.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cache.c_str(), &startup, &process)) {
            WaitForSingleObject(process.hProcess, 20000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        SetEnvironmentVariableA("SS_FRAMES", nullptr);
        SetEnvironmentVariableA("SS_DUMP", nullptr);
        SetEnvironmentVariableA("SS_INPUT", nullptr);
        SetEnvironmentVariableA("SS_SKIP", nullptr);
        DeleteFileA((cache + "\\ss_video_dump.bin").c_str());       /* only the picture is kept */
        DeleteFileA((cache + "\\ss_ram_dump.bin").c_str());
        if (FILE *file = fopen(mark.c_str(), "w")) {
            fputs(rom.c_str(), file);
            fclose(file);
        }
    }
    load_title();
}

static void choose_rom(const Game &game, SDL_Window *window)
{
    char path[MAX_PATH] = "";
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    dialog.lpstrFilter = "Game Boy Advance ROM (*.gba)\0*.gba\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof path;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog))
        return;
    settings[std::string("rom_") + game.id] = path;
    save_settings();
    check_rom(game);
}

/* ---- online: how far is the server ---- */

#define DEFAULT_SERVER "45.8.207.150:47900"

static int ping_ms = -2;        /* -2 not measured, -1 no answer */

/* Round trip to the relay: the best of five echoes (relay/ss_relay.c, packet type 5). */
/* ---- online: which server ----
 * Three ways to meet. The official server is the default and needs nothing. "Host on this PC" starts the relay
 * that comes with the port (ss_relay.exe, the same program the official server runs) and shows the addresses the
 * others type; "Connect by IP" is the other side of that. The setting is `connection` (official, host, ip). */
static std::string normalized_endpoint(const std::string &address)
{
    char host[SS_NET_HOST_CAPACITY];
    int port;
    if (!ss_net_parse_endpoint(address.c_str(), host, sizeof host, &port))
        return "";
    return std::string(host) + ":" + std::to_string(port);
}

/* The address the game is given. */
static std::string server_address()
{
    std::string how = setting("connection", "official");
    if (how == "host")
        return "127.0.0.1:47900";
    if (how == "ip")
        return normalized_endpoint(setting("server_ip"));
    return DEFAULT_SERVER;
}

/* This PC's addresses on its networks (not the loopback one), for the others to type. */
static std::vector<std::string> local_addresses()
{
    std::vector<std::string> list;
    WSADATA data;
    char name[256];
    addrinfo hints = {}, *found = nullptr;
    WSAStartup(MAKEWORD(2, 2), &data);
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) == 0 && getaddrinfo(name, nullptr, &hints, &found) == 0) {
        for (addrinfo *one = found; one; one = one->ai_next) {
            char text[64];
            const sockaddr_in *in = (const sockaddr_in *)one->ai_addr;
            if (InetNtopA(AF_INET, (void *)&in->sin_addr, text, sizeof text) && strncmp(text, "127.", 4) != 0
                && std::find(list.begin(), list.end(), text) == list.end())
                list.push_back(text);
        }
        freeaddrinfo(found);
    }
    return list;
}

/* The relay on this PC: started hidden, and it ends by itself after ten minutes without a packet. */
static bool relay_running()
{
    HANDLE mark = OpenMutexA(SYNCHRONIZE, FALSE, "ShiningSourceRelay47900");
    if (mark)
        CloseHandle(mark);
    /* the relay holds the port: a second bind fails while it runs */
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in any = {};
    any.sin_family = AF_INET;
    any.sin_port = htons(47900);
    bool taken = bind(sock, (sockaddr *)&any, sizeof any) != 0;
    closesocket(sock);
    return taken;
}

static bool start_relay()
{
    if (relay_running())
        return true;
    std::string program = folder + "ss_relay.exe", command = "\"" + program + "\" 47900";
    STARTUPINFOA startup = {};
    PROCESS_INFORMATION process = {};
    startup.cb = sizeof startup;
    SetEnvironmentVariableA("SS_RELAY_IDLE_EXIT", "600");
    BOOL made = CreateProcessA(program.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, folder.c_str(), &startup, &process);
    SetEnvironmentVariableA("SS_RELAY_IDLE_EXIT", nullptr);
    if (!made)
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(200);
    return relay_running();
}

static void measure_ping(const std::string &server)
{
    WSADATA data;
    char host[SS_NET_HOST_CAPACITY];
    int port;
    addrinfo hints = {}, *found = nullptr;
    ping_ms = -1;
    if (!ss_net_parse_endpoint(server.c_str(), host, sizeof host, &port))
        return;
    WSAStartup(MAKEWORD(2, 2), &data);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, std::to_string(port).c_str(), &hints, &found) || !found)
        return;
    SOCKET sock = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    DWORD wait = 400;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof wait);
    for (int i = 0; i < 5; i++) {
        unsigned char packet[60] = { 'S', 'S', 'N', '1', 5 }, answer[64];
        LARGE_INTEGER frequency, before, after;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        sendto(sock, (const char *)packet, sizeof packet, 0, found->ai_addr, (int)found->ai_addrlen);
        if (recv(sock, (char *)answer, sizeof answer, 0) == 60) {
            QueryPerformanceCounter(&after);
            int ms = (int)((after.QuadPart - before.QuadPart) * 1000 / frequency.QuadPart);
            if (ping_ms < 0 || ms < ping_ms)
                ping_ms = ms;
        }
    }
    closesocket(sock);
    freeaddrinfo(found);
}

/* ---- controls: which key and which controller button is each of the console's buttons ---- */

static const char *const BUTTON_NAMES[10] = { "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L" };
static const char *const BUTTON_NAMES_ES[10] = { "A", "B", "Select", "Start", "Derecha", "Izquierda", "Arriba", "Abajo", "R", "L" };
static const int DEFAULT_KEYS[10] = { SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT,
                                      SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V, SDL_SCANCODE_C };
static int capture_key = -1, capture_pad = -1;      /* the entry waiting for a press, or -1 */
static SDL_GamepadButton captured_button = SDL_GAMEPAD_BUTTON_INVALID;
static SDL_JoystickID captured_device;

static ss_pad_profile controller_profile()
{
    return ss_pad_profile_from_setting(setting("pad_profile").c_str());
}

static void bindings(const char *name, const int *defaults, int *out, int lowest = 1)
{
    std::string text = setting(name);
    const char *at = text.c_str();
    for (int i = 0; i < 10; i++) {
        char *end;
        long value = strtol(at, &end, 10);
        out[i] = end != at && value >= lowest ? (int)value : defaults[i];     /* controller buttons start at 0 */
        at = *end == ',' ? end + 1 : end;
    }
}

static void store_bindings(const char *name, const int *values)
{
    std::string text;
    for (int i = 0; i < 10; i++)
        text += (i ? "," : "") + std::to_string(values[i]);
    settings[name] = text;
    save_settings();
}

/* Called for every event while the Controls page waits for a press. */
static void capture_event(const SDL_Event &event)
{
    int values[10];
    if (capture_key >= 0 && event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.scancode != SDL_SCANCODE_ESCAPE) {        /* Esc cancels; in the game it always opens the options menu */
            bindings("keys", DEFAULT_KEYS, values);
            values[capture_key] = (int)event.key.scancode;
            store_bindings("keys", values);
        }
        capture_key = -1;
    }
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP && event.gbutton.which == captured_device &&
        event.gbutton.button == captured_button)
        captured_button = SDL_GAMEPAD_BUTTON_INVALID;
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED && event.gdevice.which == captured_device)
        captured_button = SDL_GAMEPAD_BUTTON_INVALID;
    if (capture_pad >= 0 && (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)) {
        SDL_JoystickID device = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? event.gbutton.which : event.gaxis.which;
        SDL_Gamepad *pad = SDL_GetGamepadFromID(device);
        const ss_pad_profile profile = controller_profile();
        const bool gamecube = ss_pad_uses_gamecube(pad, profile);
        SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            button = (SDL_GamepadButton)event.gbutton.button;
        else if (gamecube)
            button = ss_pad_capture_trigger(pad, (SDL_GamepadAxis)event.gaxis.axis, event.gaxis.value);
        if (button == SDL_GAMEPAD_BUTTON_INVALID) return;
        int defaults[10];
        ss_pad_defaults(pad, profile, defaults);
        const char *key = gamecube ? "pad_gamecube" : "pad";
        bindings(key, defaults, values, 0);
        values[capture_pad] = (int)button;
        store_bindings(key, values);
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            captured_button = button;
            captured_device = device;
        }
        capture_pad = -1;
    }
}

static void controller_cancel(const SDL_Event &event)
{
    if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.type != SDL_EVENT_GAMEPAD_BUTTON_UP) return;
    SDL_Gamepad *pad = SDL_GetGamepadFromID(event.gbutton.which);
    if (!ss_pad_uses_gamecube(pad, controller_profile())) return;
    const auto button = ss_pad_face_button(pad, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_WEST);
    if (button == SDL_GAMEPAD_BUTTON_EAST || event.gbutton.button != button) return;
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP || capture_pad < 0)
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
}

/* ---- RetroAchievements ----
 * The user signs in with their own account; the client (launcher/achievements, on rcheevos) asks the site for the
 * achievement list of the ROM they chose and shows it. The password goes to retroachievements.org and nowhere
 * else, and is not kept: what stays on this PC is the session token the site returns, protected for this Windows
 * user. Nothing is evaluated here: the game does that (see docs/ROADMAP.md for what is and is not sent). */
static KhRa *ra;
static std::string ra_rom;          /* the ROM the list was asked for */

/* ---- Rebel mode and the records the game keeps on this PC ----
 * The game evaluates achievements itself (runtime/achievements/ach_host.c). Besides what goes to the account it
 * keeps two records next to the launcher, one line per unlock ("id <tab> unix time <tab> title"):
 * local_record.txt for normal runs and rebel_record.txt for runs with cheats. Retired custom ids are ignored. */
struct Cheat { const char *id, *english, *spanish; };
static const Cheat cheats[] = {
    { "hp", "Health never runs out", "La vida no se agota" },
    { "sp", "SP never runs out (the energy for skills and magic)", "Los SP no se agotan (la energ\xC3\xAD" "a de t\xC3\xA9" "cnicas y magia)" },
    { "nodeath", "Cannot be defeated: damage counts, but health stops at 1", "No puedes caer: el da\xC3\xB1o cuenta, pero la vida se detiene en 1" },
    { "gold", "Full purse: 100,000 gold in the bag at all times", "Bolsa llena: 100.000 de oro en la bolsa en todo momento" },
};

/* What the game is given in SS_CHEATS: empty unless Rebel mode is on and at least one cheat is ticked. */
static std::string cheat_list()
{
    std::string list;
    if (setting("rebel", "0") != "1")
        return list;
    for (const Cheat &cheat : cheats)
        if (setting((std::string("cheat_") + cheat.id).c_str(), "0") == "1")
            list += (list.empty() ? "" : ",") + std::string(cheat.id);
    return list;
}

struct Unlock { uint32_t id; long long when; std::string title; };

static std::vector<Unlock> read_record(const char *name)
{
    std::vector<Unlock> record;
    char line[512];
    if (FILE *file = fopen((folder + name).c_str(), "r")) {
        while (fgets(line, sizeof line, file)) {
            Unlock unlock;
            char *end, *tab;
            unlock.id = (uint32_t)strtoul(line, &end, 10);
            unlock.when = strtoll(end, &tab, 10);
            if (!unlock.id || unlock.id >= SS_ACH_WARNING_ID || *tab != '\t')
                continue;
            unlock.title = tab + 1;
            while (!unlock.title.empty() && (unlock.title.back() == '\n' || unlock.title.back() == '\r'))
                unlock.title.pop_back();
            record.push_back(unlock);
        }
        fclose(file);
    }
    return record;
}

static const Unlock *in_record(const std::vector<Unlock> &record, uint32_t id)
{
    for (const Unlock &unlock : record)
        if (unlock.id == id)
            return &unlock;
    return nullptr;
}

static std::string day_of(long long when)
{
    char day[32] = "";
    time_t seconds = (time_t)when;
    if (struct tm *local = localtime(&seconds))
        strftime(day, sizeof day, "%Y-%m-%d", local);
    return day;
}

/* The RetroAchievements entries of a record (the port's own are shown in their section). */
static void record_list(const std::vector<Unlock> &record, const ImVec4 &colour, const ImVec4 &dim)
{
    int shown = 0;
    for (const Unlock &unlock : record)
        if (unlock.id < SS_ACH_WARNING_ID) {
            ImGui::TextColored(colour, "* %s", unlock.title.c_str());
            ImGui::SameLine();
            ImGui::TextColored(dim, "%s", day_of(unlock.when).c_str());
            shown++;
        }
    if (!shown)
        ImGui::TextColored(dim, "%s", text("(nothing yet)", "(nada todav\xC3\xAD" "a)"));
}

static void rebel_section(const std::vector<Unlock> &rebel, const ImVec4 &dim, const ImVec4 &pink)
{
    bool on = setting("rebel", "0") == "1";
    ImGui::TextWrapped("%s", text("Rebel mode is for playing with cheats. A game started with a cheat on is a Rebel run until it closes: nothing is sent to RetroAchievements, "
                                  "the same achievements are checked on this PC and kept in the Rebel record below, apart from everything else, and the game saves to its own file "
                                  "(shiningsource_rebel.sav, which starts as a copy of your normal save), so nothing a cheat gave reaches your normal game. Cheats pause while an online session is active.",
                                  "El modo rebelde es para jugar con trucos. Una partida iniciada con un truco activo es rebelde hasta que se cierra: no se env\xC3\xAD" "a nada a RetroAchievements, "
                                  "los mismos logros se comprueban en este PC y se guardan en el registro rebelde de abajo, aparte de todo lo dem\xC3\xA1s, y el juego guarda en su propio archivo "
                                  "(shiningsource_rebel.sav, que empieza como copia de tu partida normal), as\xC3\xAD nada de lo que dio un truco llega a tu partida normal. Los trucos se pausan durante una sesi\xC3\xB3n en l\xC3\xADnea."));
    if (ImGui::Checkbox(text("Rebel mode: enable cheats", "Modo rebelde: activar trucos"), &on)) {
        settings["rebel"] = on ? "1" : "0";
        save_settings();
    }
    ImGui::BeginDisabled(!on);
    ImGui::Indent();
    for (const Cheat &cheat : cheats) {
        std::string key = std::string("cheat_") + cheat.id;
        bool ticked = setting(key.c_str(), "0") == "1";
        if (ImGui::Checkbox(language ? cheat.spanish : cheat.english, &ticked)) {
            settings[key] = ticked ? "1" : "0";
            save_settings();
        }
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    if (on && cheat_list().empty())
        ImGui::TextColored(dim, "%s", text("No cheat is ticked: the next game is a normal one.", "No hay ning\xC3\xBAn truco marcado: la pr\xC3\xB3xima partida es normal."));
    else if (on)
        ImGui::TextColored(pink, "%s", text("The next game is a Rebel run.", "La pr\xC3\xB3xima partida es rebelde."));
    ImGui::Separator();
    ImGui::TextColored(pink, text("Rebel record: %u RetroAchievements", "Registro rebelde: %u de RetroAchievements"), (unsigned)rebel.size());
    ImGui::BeginChild("rebel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    record_list(rebel, pink, dim);
    ImGui::EndChild();
}

/* Read the same cached RA catalog the game evaluates, including offline. */
static void local_section(const std::vector<Unlock> &record, const ImVec4 &gold, const ImVec4 &dim)
{
    struct Entry { uint32_t id, points; std::string title, description; };
    static std::vector<Entry> entries;
    static std::string previous_hash;
    static unsigned age;
    std::string hash = ra ? KhRaHash(ra) : "";
    if (hash != previous_hash || age++ % 60 == 0) {
        entries.clear();
        previous_hash = hash;
        if (!hash.empty()) {
            std::vector<char> body(8u << 20);
            if (FILE *file = fopen((folder + "ra_set_" + hash + ".json").c_str(), "rb")) {
                size_t length = fread(body.data(), 1, body.size() - 1, file);
                fclose(file);
                rc_api_server_response_t response = {body.data(), length, 200};
                rc_api_fetch_game_sets_response_t sets;
                if (rc_api_process_fetch_game_sets_server_response(&sets, &response) == RC_OK && sets.response.succeeded)
                    for (uint32_t s = 0; s < sets.num_sets; ++s)
                        for (uint32_t a = 0; a < sets.sets[s].num_achievements; ++a) {
                            const auto &entry = sets.sets[s].achievements[a];
                            if (entry.category != RC_ACHIEVEMENT_CATEGORY_CORE || entry.id >= SS_ACH_WARNING_ID)
                                continue;
                            bool duplicate = false;
                            for (const auto &existing : entries) duplicate |= existing.id == entry.id;
                            if (!duplicate)
                                entries.push_back({entry.id, entry.points, entry.title ? entry.title : "", entry.description ? entry.description : ""});
                        }
                rc_api_destroy_fetch_game_sets_response(&sets);
            }
        }
    }
    unsigned earned = 0;
    for (const auto &entry : entries) earned += in_record(record, entry.id) != nullptr;
    ImGui::Text(text("%u of %u earned locally", "%u de %u conseguidos localmente"), earned, (unsigned)entries.size());
    ImGui::TextWrapped("%s", text("The included RetroAchievements catalog is checked while you play, even offline. Sign in to refresh it; local progress stays on this PC.",
                                  "El conjunto incluido de RetroAchievements se comprueba mientras juegas, incluso sin conexi\xC3\xB3n. Inicia sesi\xC3\xB3n para actualizarlo; el progreso local queda en este PC."));
    ImGui::BeginChild("local", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (entries.empty())
        ImGui::TextWrapped("%s", text("No downloaded catalog for this ROM. Sign in on the RetroAchievements tab.", "No hay conjunto descargado para esta ROM. Inicia sesi\xC3\xB3n en la pesta\xC3\xB1" "a de RetroAchievements."));
    for (const auto &entry : entries) {
        const Unlock *unlock = in_record(record, entry.id);
        ImGui::TextColored(unlock ? gold : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s %s (%u)", unlock ? "*" : "-", entry.title.c_str(), entry.points);
        if (unlock) { ImGui::SameLine(); ImGui::TextColored(dim, "%s", day_of(unlock->when).c_str()); }
        ImGui::TextWrapped("%s", entry.description.c_str());
    }
    ImGui::EndChild();
}

static void account_section(const Game &game, const ImVec4 &gold, const ImVec4 &dim, const ImVec4 &red, const ImVec4 &green);

static void achievements_page(const Game &game, const ImVec4 &gold, const ImVec4 &dim, const ImVec4 &red, const ImVec4 &green)
{
    static std::vector<Unlock> local, rebel;
    static int age;
    static int open_tab = getenv("SS_LAUNCHER_TAB") ? atoi(getenv("SS_LAUNCHER_TAB")) : -1;     /* tests: open on a given tab */
    auto chosen = [](int tab) { return open_tab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    const ImVec4 pink(1.0f, 0.42f, 0.62f, 1);
    if (age++ % 60 == 0) {              /* the game may be writing them right now */
        local = read_record("local_record.txt");
        rebel = read_record("rebel_record.txt");
    }
    std::string rom = setting((std::string("rom_") + game.id).c_str());
    if (rom != ra_rom) {            /* the list belongs to a ROM: ask again when it changes */
        std::vector<uint8_t> bytes;
        if (FILE *file = fopen(rom.c_str(), "rb")) {
            bytes.resize(32u * 1024 * 1024);
            bytes.resize(fread(bytes.data(), 1, bytes.size(), file));
            fclose(file);
        }
        KhRaSetRom(ra, bytes.empty() ? nullptr : bytes.data(), bytes.size());
        ra_rom = rom;
    }
    ImGui::TextColored(gold, "%s", text("Achievements", "Logros"));
    if (!ImGui::BeginTabBar("achievements"))
        return;
    if (ImGui::BeginTabItem(text("RetroAchievements account", "Cuenta de RetroAchievements"), nullptr, chosen(0))) {
        account_section(game, gold, dim, red, green);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(text("Local achievements", "Logros locales"), nullptr, chosen(1))) {
        local_section(local, gold, dim);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(text("Rebel mode", "Modo rebelde"), nullptr, chosen(3))) {
        rebel_section(rebel, dim, pink);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (age > 2)
        open_tab = -1;                  /* from then on the user chooses */
}

static void account_section(const Game &game, const ImVec4 &gold, const ImVec4 &dim, const ImVec4 &red, const ImVec4 &green)
{
    static char username[64], password[128];
    static bool remember = true;
    rc_client_t *client = KhRaClient(ra);
    const rc_client_user_t *user = client ? rc_client_get_user_info(client) : nullptr;
    if (!ra) {
        ImGui::TextColored(red, "%s", text("The achievements client could not be started.", "No se pudo iniciar el cliente de logros."));
        return;
    }
    if (user) {
        ImGui::TextColored(green, text("Signed in as %s", "Sesi\xC3\xB3n iniciada como %s"), user->display_name);
        ImGui::SameLine();
        if (soul_ui::button(text("Sign out", "Cerrar sesi\xC3\xB3n")))
            KhRaLogout(ra);
        {
            /* off unless the player turns it on: an unlock on the account cannot be taken back, and the set was
             * written for the cartridge in an approved emulator, not checked against this port */
            bool submit = setting("ra_submit", "0") == "1";
            if (ImGui::Checkbox(text("Send my unlocks to this account", "Enviar mis logros a esta cuenta"), &submit)) {
                settings["ra_submit"] = submit ? "1" : "0";
                save_settings();
            }
            ImGui::TextWrapped("%s", text("Off: the list is still followed while you play and kept on this PC. On: unlocks are sent as softcore. "
                                          "RetroAchievements only counts programs it has approved, and this port is not one of them yet: use it at your own judgement.",
                                          "Desactivado: la lista se sigue igualmente mientras juegas y se guarda en este PC. Activado: los logros se env\xC3\xAD" "an como softcore. "
                                          "RetroAchievements solo cuenta los programas que ha aprobado y este port a\xC3\xBA" "n no lo es: \xC3\xBAsalo bajo tu criterio."));
        }
    } else {
        ImGui::TextWrapped("%s", text("Sign in with your RetroAchievements account to see this game's achievements.",
                                      "Inicia sesi\xC3\xB3n con tu cuenta de RetroAchievements para ver los logros de este juego."));
        ImGui::SetNextItemWidth(260);
        ImGui::InputText(text("User", "Usuario"), username, sizeof username);
        ImGui::SetNextItemWidth(260);
        ImGui::InputText(text("Password", "Contrase\xC3\xB1" "a"), password, sizeof password, ImGuiInputTextFlags_Password);
        ImGui::Checkbox(text("Keep me signed in on this PC", "Mantener la sesi\xC3\xB3n en este PC"), &remember);
        ImGui::BeginDisabled(!username[0] || !password[0] || KhRaBusy(ra));
        if (soul_ui::button(text("Sign in", "Iniciar sesi\xC3\xB3n"))) {
            KhRaSetEnabled(ra, 1);
            KhRaLogin(ra, username, password, remember);
            SecureZeroMemory(password, sizeof password);
        }
        ImGui::EndDisabled();
    }
    if (KhRaStatus(ra)[0])
        ImGui::TextColored(KhRaError(ra) ? red : dim, "%s", KhRaStatus(ra));
    ImGui::Separator();
    rc_client_achievement_list_t *list = client && user ? rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                                            RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE) : nullptr;
    if (!list || !list->num_buckets) {
        ImGui::TextColored(dim, "%s", user ? text("No achievement list yet for this ROM.", "A\xC3\xBAn no hay lista de logros para esta ROM.")
                                           : text("The list appears after signing in.", "La lista aparece al iniciar sesi\xC3\xB3n."));
    } else {
        unsigned total = 0, unlocked = 0, points = 0;
        for (uint32_t b = 0; b < list->num_buckets; b++)
            for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++) {
                const rc_client_achievement_t *a = list->buckets[b].achievements[i];
                if (a->id >= SS_ACH_WARNING_ID) continue;
                total++;
                points += a->points;
                unlocked += a->unlocked != 0;
            }
        ImGui::Text(text("%u of %u unlocked on your account, %u points in the set", "%u de %u desbloqueados en tu cuenta, %u puntos en el conjunto"), unlocked, total, points);
        ImGui::TextColored(dim, "%s", text("Earned on your account by playing signed in and without cheats. What you earn without an account, and Rebel mode, are kept apart and never touch this.",
                                           "Se ganan en tu cuenta jugando con la sesi\xC3\xB3n iniciada y sin trucos. Lo que consigues sin cuenta, y el modo rebelde, van aparte y nunca tocan esto."));
        ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (uint32_t b = 0; b < list->num_buckets; b++)
            for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++) {
                const rc_client_achievement_t *a = list->buckets[b].achievements[i];
                if (a->id >= SS_ACH_WARNING_ID) continue;
                ImGui::TextColored(a->unlocked ? gold : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s %s  (%u)", a->unlocked ? "*" : "-", a->title, a->points);
                ImGui::TextColored(dim, "    %s", a->description);
            }
        ImGui::EndChild();
    }
    if (list)
        rc_client_destroy_achievement_list(list);
}

/* ---- start the game ---- */

#include "qol.h"                    /* the "Quality of life" page: qol_page, qol_environment */

/* ---------------------------------------------------------------- saves */

/* The game keeps its save beside the program (shiningsource.sav, the previous one as .bak; Rebel mode has its
 * own pair). This page shows them and keeps dated copies in the folder "saves": a copy is made on request, and
 * putting one back first copies the present save there too, so a restore never loses anything. */

struct SaveFile { std::string name; long long size, when; };

static bool file_facts(const std::string &path, SaveFile &out)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    ULARGE_INTEGER time;
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    out.size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    out.when = (long long)(time.QuadPart / 10000000ULL) - 11644473600LL;
    return true;
}

static std::string minute_of(long long when)
{
    time_t moment = (time_t)when;
    struct tm *local = localtime(&moment);
    char line[40] = "";
    if (local)
        strftime(line, sizeof line, "%Y-%m-%d %H:%M", local);
    return line;
}

static std::vector<SaveFile> kept_copies()
{
    std::vector<SaveFile> list;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA((folder + "saves\\*.sav").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return list;
    do {
        SaveFile entry;
        entry.name = found.cFileName;
        if (file_facts(folder + "saves\\" + entry.name, entry))
            list.push_back(entry);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    for (size_t i = 1; i < list.size(); i++)            /* newest first */
        for (size_t j = i; j > 0 && list[j].when > list[j - 1].when; j--)
            std::swap(list[j], list[j - 1]);
    return list;
}

/* Copy `source` (a file beside the program) into saves\ under a dated name. Returns the name, or "" on failure. */
static std::string keep_copy(const char *source, const char *label)
{
    SYSTEMTIME now;
    char name[96];
    GetLocalTime(&now);
    CreateDirectoryA((folder + "saves").c_str(), nullptr);
    snprintf(name, sizeof name, "%s-%04d%02d%02d-%02d%02d%02d.sav", label, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return CopyFileA((folder + source).c_str(), (folder + "saves\\" + name).c_str(), TRUE) ? name : "";
}

/* Tests: SS_LAUNCHER_TEST=keep, or restore (a comma list) presses that button of this page once, as a click
 * would: "keep" the first "Keep a copy now", "restore" the newest copy's "Put back" and then its "Yes". */
static bool test_press(const char *what)
{
    static std::string done;
    const char *list = getenv("SS_LAUNCHER_TEST");
    std::string token = std::string(",") + what + ",";
    if (!list || (std::string(",") + list + ",").find(token) == std::string::npos || done.find(token) != std::string::npos)
        return false;
    done += token;
    return true;
}

static void saves_page(const ImVec4 &gold, const ImVec4 &dim, const ImVec4 &red, const ImVec4 &green)
{
    static std::string answer, ask_restore;
    static bool answer_good;
    static const struct { const char *file, *label, *english, *spanish; } kinds[] = {
        { "shiningsource.sav", "normal", "Your save", "Tu partida" },
        { "shiningsource_rebel.sav", "rebel", "Rebel mode save", "Partida del modo rebelde" },
    };

    ImGui::TextColored(gold, "%s", text("Saves", "Partidas"));
    ImGui::TextWrapped("%s", text("The game saves beside the program, as the cartridge did in its memory, and keeps the save before the last one as a backup. Here you can keep dated copies and put one back.",
                                  "El juego guarda junto al programa, como el cartucho en su memoria, y conserva la partida anterior a la \xC3\xBAltima como respaldo. Aqu\xC3\xAD puedes guardar copias con fecha y recuperar una."));
    ImGui::Spacing();
    for (const auto &kind : kinds) {
        SaveFile now, before;
        bool have = file_facts(folder + kind.file, now), have_before = file_facts(folder + kind.file + ".bak", before);
        ImGui::PushID(kind.file);
        ImGui::TextColored(gold, "%s", text(kind.english, kind.spanish));
        if (have)
            ImGui::Text("%s    %s", kind.file, minute_of(now.when).c_str());
        else
            ImGui::TextColored(dim, "%s", text("None yet.", "A\xC3\xBAn no hay."));
        if (have_before)
            ImGui::TextColored(dim, text("Backup of the one before: %s", "Respaldo de la anterior: %s"), minute_of(before.when).c_str());
        ImGui::BeginDisabled(!have);
        if (soul_ui::button(text("Keep a copy now", "Guardar una copia ahora")) || (have && &kind == &kinds[0] && test_press("keep"))) {
            std::string name = keep_copy(kind.file, kind.label);
            answer_good = !name.empty();
            answer = answer_good ? std::string(text("Copy kept: ", "Copia guardada: ")) + name : text("The copy could not be written.", "No se pudo escribir la copia.");
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (soul_ui::button(text("Open the folder", "Abrir la carpeta")))
        ShellExecuteA(nullptr, "open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (!answer.empty())
        ImGui::TextColored(answer_good ? green : red, "%s", answer.c_str());

    ImGui::Separator();
    ImGui::TextColored(gold, "%s", text("Copies kept", "Copias guardadas"));
    std::vector<SaveFile> copies = kept_copies();
    if (copies.empty())
        ImGui::TextColored(dim, "%s", text("None. They are kept in the folder \"saves\" beside the program.", "Ninguna. Se guardan en la carpeta \"saves\" junto al programa."));
    for (const SaveFile &copy : copies) {
        bool rebel = copy.name.compare(0, 5, "rebel") == 0;
        ImGui::PushID(copy.name.c_str());
        if (ImGui::SmallButton(text("Put back", "Recuperar")) || (&copy == &copies[0] && test_press("restore")))
            ask_restore = copy.name;
        ImGui::SameLine();
        ImGui::Text("%s", minute_of(copy.when).c_str());
        ImGui::SameLine();
        ImGui::TextColored(dim, "%s  (%s)", copy.name.c_str(), rebel ? text("Rebel mode", "modo rebelde") : "normal");
        ImGui::PopID();
    }
    if (!ask_restore.empty()) {
        bool rebel = ask_restore.compare(0, 5, "rebel") == 0;
        const auto &kind = kinds[rebel];
        ImGui::Separator();
        ImGui::TextWrapped(text("Put %s back as %s? The present save is first kept as a copy here. Close the game before doing this.",
                                "\xC2\xBFRecuperar %s como %s? La partida actual se guarda antes como copia aqu\xC3\xAD. Cierra el juego antes de hacerlo."),
                           ask_restore.c_str(), text(kind.english, kind.spanish));
        if (soul_ui::button(text("Yes, put it back", "S\xC3\xAD, recuperar")) || test_press("restore-yes")) {
            SaveFile present;
            bool safe = !file_facts(folder + kind.file, present) || !keep_copy(kind.file, rebel ? "rebel-before-restore" : "normal-before-restore").empty();
            answer_good = safe && CopyFileA((folder + "saves\\" + ask_restore).c_str(), (folder + kind.file).c_str(), FALSE);
            answer = answer_good ? std::string(text("Put back: ", "Recuperada: ")) + ask_restore
                   : safe ? text("The save could not be replaced (is the game running?).", "No se pudo reemplazar la partida (\xC2\xBF" "est\xC3\xA1 el juego abierto?).")
                          : text("The present save could not be copied first; nothing was changed.", "No se pudo copiar antes la partida actual; no se cambi\xC3\xB3 nada.");
            ask_restore.clear();
        }
        ImGui::SameLine();
        if (soul_ui::button("No"))
            ask_restore.clear();
    }
}

static std::string launch_error;

static bool launched;            /* the game was started: the launcher's work is done and it closes */

static int play(const Game &game, const std::string &rom, const wchar_t *explicit_rom = nullptr)
{
    const std::string endpoint = server_address();
    if (endpoint.empty()) {
        launch_error = text("Enter a valid host name or IPv4 address in Online settings.",
                            "Escribe un nombre de host o una direcci\xC3\xB3n IPv4 v\xC3\xA1lida en los ajustes en l\xC3\xADnea.");
        return 1;
    }
    if (!explicit_rom)
        save_settings();        /* always, whether or not the player pressed "Save settings" */
    std::string host = folder + game.host;
    const std::string command = "\"" + host + "\"" + (explicit_rom ? "" : " \"" + rom + "\"");
    STARTUPINFOA startup = {};
    PROCESS_INFORMATION process = {};
    std::string view = setting("view", "auto");

    bool classic = setting("mode", "enhanced") == "classic";
    SetEnvironmentVariableA("SS_MODE", classic ? "classic" : nullptr);
    SetEnvironmentVariableA("SS_BORDER", classic ? setting("border", "16").c_str() : nullptr);
    SetEnvironmentVariableA("SS_BORDER_STYLE", classic ? setting("border_style", "4").c_str() : nullptr);
    SetEnvironmentVariableA("SS_VIEW", view == "auto" || classic ? nullptr : view.c_str());
    SetEnvironmentVariableA("SS_HUD_SCALE", !classic && setting("hud_scale", "0") == "1" ? "1" : nullptr);
    SetEnvironmentVariableA("SS_VSYNC", setting("vsync", "1").c_str());
    SetEnvironmentVariableA("SS_LANG", language ? "es" : "en");
    SetEnvironmentVariableA("SS_SKIP", setting("skip", "1") == "0" ? "0" : nullptr);
    SetEnvironmentVariableA("SS_VOLUME", setting("volume", "100").c_str());
    SetEnvironmentVariableA("SS_KEYS", setting("keys").empty() ? nullptr : setting("keys").c_str());
    SetEnvironmentVariableA("SS_PAD", setting("pad").empty() ? nullptr : setting("pad").c_str());
    qol_environment();          /* the experimental quality-of-life options (qol.h) */
    const std::string room = setting("direct_room", "0") == "1" ? setting("room") : "";
    if (setting("connection", "official") == "host")
        start_relay();          /* hosting: the server must be there before the game looks for it */
    SetEnvironmentVariableA("SS_NET", endpoint.c_str());
    SetEnvironmentVariableA("SS_NET_DEFERRED", room.empty() ? "1" : nullptr);
    SetEnvironmentVariableA("SS_NET_ROOM", room.empty() ? nullptr : room.c_str());
    SetEnvironmentVariableA("SS_NET_DELAY", nullptr);       /* the direct transport has no delay to set */
    /* achievements: where the remembered sign-in, the game's list and the records are (never a password), and
     * the cheats of a Rebel run */
    SetEnvironmentVariableA("SS_RA_DIR", folder.substr(0, folder.size() - 1).c_str());
    SetEnvironmentVariableA("SS_CHEATS", cheat_list().empty() ? nullptr : cheat_list().c_str());
    SetEnvironmentVariableA("SS_RA_SUBMIT", setting("ra_submit", "0") == "1" ? "1" : nullptr);
    startup.cb = sizeof startup;
    launch_error.clear();
    /* Keep the picker path in UTF-16 across the process boundary. */
    if (explicit_rom && !SetEnvironmentVariableW(L"SS_LAUNCHER_ROM", explicit_rom)) {
        launch_error = "The game path could not be passed to the game program.";
        return 1;
    }
    /* no console window beside the game: its messages go to the files it writes */
    DWORD exit_code = 0;
    do {
        std::string args = command;       /* CreateProcess may modify the buffer. */
        if (!CreateProcessA(host.c_str(), &args[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process)) {
            launch_error = text("The game program could not be started: ", "No se pudo iniciar el programa del juego: ") + host;
            return 1;
        }
        CloseHandle(process.hThread);
        if (explicit_rom && (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
                             !GetExitCodeProcess(process.hProcess, &exit_code)))
            exit_code = 1;
        CloseHandle(process.hProcess);
    } while (explicit_rom && exit_code == SS_EXIT_RESTART);
    launched = true;
    return (int)exit_code;
}

static void play(const Game &game)
{
    play(game, setting((std::string("rom_") + game.id).c_str()));
}

/* ---- interface ---- */

static void interface_frame(SDL_Window *window, bool &running)
{
    static int selected = 0, page = 0;
    static bool first = true;
    static bool rail_active = false, content_active = false;
    static rom_browser::Picker picker;
    const bool opened = first;
    if (first && getenv("SS_LAUNCHER_PAGE"))
        page = std::clamp(atoi(getenv("SS_LAUNCHER_PAGE")), 0, 8);
    first = false;
    const Game &game = games[selected];
    std::string rom = setting((std::string("rom_") + game.id).c_str());
    const ImVec4 gold(.91f, .78f, .52f, 1), red(.99f, .55f, .48f, 1), green(.64f, .84f, .57f, 1), dim(.66f, .73f, .66f, 1);

    soul_ui::backdrop();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("ShiningSource", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    soul_ui::chrome(window, running);
    ImGui::SetCursorPos(ImVec2(14, soul_ui::title_height + 14));

    const bool switching = !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool enter_rail = opened || (switching && content_active && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft));
    const bool enter_content = switching && rail_active && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight);
    if (enter_content || (enter_rail && !opened)) ImGui::SetNavCursorVisible(true);
    if (enter_rail) ImGui::SetNextWindowFocus();
    ImGui::BeginChild("navigation", ImVec2(184, -14), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    const char *pages_en[] = { "Home", "Video", "Sound", "Controls", "Online", "Achievements", "Quality of life", "Saves", "About" };
    const char *pages_es[] = { "Inicio", "V\xC3\xAD" "deo", "Sonido", "Controles", "En l\xC3\xADnea", "Logros", "Calidad de vida", "Partidas", "Acerca de" };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    for (int i = 0; i < 9; ++i) {
        ImGui::PushID(i);
        if (enter_rail && i == page) ImGui::SetKeyboardFocusHere();
        if (soul_ui::button(language ? pages_es[i] : pages_en[i], ImVec2(-1, 33), false, page == i, true, i)) {
            page = i;
            capture_key = capture_pad = -1;
        }
        if (i == page && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    const float bottom_y = ImGui::GetWindowHeight() - 119;
    if (ImGui::GetCursorPosY() < bottom_y) ImGui::SetCursorPosY(bottom_y);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##language", &language, "English\0Espa\xC3\xB1ol\0")) {
        settings["language"] = language ? "es" : "en";
        save_settings();
    }
    const bool playable = game.available && rom_status == "ok";
    ImGui::BeginDisabled(!playable);
    bool start_pressed = soul_ui::button(text("Play", "Jugar"), ImVec2(-1, 41), true);
    ImGui::EndDisabled();
    ImGui::TextColored(dim, "%s", text("D-pad / A to select", "Cruceta / A elige"));
    rail_active = ImGui::IsWindowFocused();
    ImGui::EndChild();
    ImGui::SameLine();

    if (enter_content) ImGui::SetNextWindowFocus();
    ImGui::BeginChild("content", ImVec2(-14, -14), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (page && game.available) soul_ui::section(language ? pages_es[page] : pages_en[page]);
    if (enter_content) ImGui::SetKeyboardFocusHere();
    if (!game.available || page == 0) {
        soul_ui::banner(game.title, game.available ? game.region : text("In development", "En desarrollo"), game.available);
        if (game.available) {
            static const auto game_language = [] {
                const std::string path = folder + SS_GAME_LANGUAGE_FILE;
                const auto current = ss_game_language_preference_at(path.c_str());
                return current == SS_LANG_UNCHOSEN ? ss_game_language_preference_at((path + ".bak").c_str()) : current;
            }();
            const char *language_names[] = { "English", "Fran\xC3\xA7" "ais", "Deutsch", "Italiano", "Espa\xC3\xB1ol" };
            ImGui::TextColored(dim, "%s %s", text("Game language", "Idioma del juego"), game_language == SS_LANG_UNCHOSEN
                ? text("(to be chosen)", "(por escoger)") : language_names[game_language]);
            ImGui::Spacing();
            ImGui::TextColored(gold, "%s", text("Cartridge file", "Archivo del cartucho"));
            if (rom_status == "ok") {
                ImGui::TextColored(green, "%s", text("European ROM verified", "ROM europea verificada"));
                ImGui::TextWrapped("%s", std::filesystem::path(rom).filename().u8string().c_str());
            } else if (rom_status == "wrong") {
                ImGui::TextColored(red, "%s", text("Choose an unmodified European ROM (AU2P).", "Elige una ROM europea original (AU2P)."));
            } else if (rom_status == "missing") {
                ImGui::TextColored(red, "%s", text("The previous ROM could not be found.", "No se encuentra la ROM anterior."));
            } else {
                ImGui::TextWrapped("%s", text("Choose your Shining Soul II European .gba file.", "Elige tu archivo .gba europeo de Shining Soul II."));
            }
            if (soul_ui::button(text("Browse for ROM", "Buscar ROM"), ImVec2(0, 39), !playable))
                picker.open(rom, folder);
            ImGui::SameLine();
            if (soul_ui::button(text("Windows file picker", "Selector de Windows"), ImVec2(0, 39))) choose_rom(game, window);
            const std::string choice = picker.draw(text("Choose a .gba cartridge file", "Elige un archivo de cartucho .gba"),
                text("Parent folder", "Carpeta anterior"), text("Drives", "Unidades"), text("Cancel", "Cancelar"),
                text("No folders or .gba files", "Sin carpetas ni archivos .gba"));
            if (!choice.empty()) {
                settings[std::string("rom_") + game.id] = choice;
                save_settings();
                check_rom(game);
            }
            if (playable && !cheat_list().empty())
                ImGui::TextColored(gold, "%s", text("Rebel mode is active. It uses a separate save.", "El modo rebelde usa una partida aparte."));
            if (!launch_error.empty()) ImGui::TextColored(red, "%s", launch_error.c_str());
            if (playable && ImGui::CollapsingHeader(text("Cartridge preview", "Vista previa del cartucho"))) {
                make_title(game);
                if (title_texture)
                    ImGui::Image(soul_ui::id(title_texture), ImVec2(240, 160));
            }
        }
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo(text("Game", "Juego"), game.title)) {
            for (int i = 0; i < (int)(sizeof games / sizeof games[0]); ++i) {
                if (ImGui::Selectable(games[i].title, selected == i)) {
                    selected = i;
                    check_rom(games[i]);
                }
            }
            ImGui::EndCombo();
        }
    } else if (page == 1) {
        static const char *values[] = { "auto", "240", "284", "320", "480" };
        const char *labels_en[] = { "Automatic: fill the window", "Original 3:2 (240 columns)", "16:9 (284 columns)", "Wider (320 columns)", "Maximum (480 columns)" };
        const char *labels_es[] = { "Autom\xC3\xA1tico: llenar la ventana", "Original 3:2 (240 columnas)", "16:9 (284 columnas)", "M\xC3\xA1s ancho (320 columnas)", "M\xC3\xA1ximo (480 columnas)" };
        std::string view = setting("view", "auto");
        int window_mode = ss_window_mode_from_settings(setting("window_mode").c_str(), setting("fullscreen").c_str());
        bool classic = setting("mode", "enhanced") == "classic";
        ImGui::SetNextItemWidth(290);
        if (ImGui::Combo(text("Window mode", "Modo de ventana"), &window_mode,
                         language ? "Ventana\0Sin bordes\0Pantalla completa exclusiva\0"
                                  : "Windowed\0Borderless\0Exclusive fullscreen\0")) {
            settings["window_mode"] = ss_window_mode_name((ss_window_mode)window_mode);
            settings.erase("fullscreen");
            save_settings();
        }
        bool wide_pro = setting("wide_pro", "1") != "0";
        ImGui::BeginDisabled(classic);
        if (ImGui::Checkbox(text("Wide Pro for title screens", "Wide Pro en las pantallas de inicio"), &wide_pro)) {
            settings["wide_pro"] = wide_pro ? "1" : "0";
            save_settings();
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextColored(gold, "%s", text("Screen mode", "Modo de pantalla"));
        if (ImGui::RadioButton(text("Classic: the console's picture at a whole scale, with a border", "Cl\xC3\xA1sico: la imagen de la consola a escala entera, con borde"), classic)) {
            settings["mode"] = "classic";
            save_settings();
        }
        if (ImGui::RadioButton(text("Enhanced: the picture adapts to the window", "Mejorado: la imagen se adapta a la ventana"), !classic)) {
            settings["mode"] = "enhanced";
            save_settings();
        }
        ImGui::Spacing();
        if (classic) {
            int size = atoi(setting("border", "16").c_str()), look = atoi(setting("border_style", "4").c_str());
            ImGui::TextColored(gold, "%s", text("Border", "Borde"));
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderInt(text("Thickness", "Grosor"), &size, 0, 64, text("%d pixels", "%d p\xC3\xADxeles"))) {
                settings["border"] = std::to_string(size);
                save_settings();
            }
            ImGui::SetNextItemWidth(260);
            if (ImGui::Combo(text("Look", "Aspecto"), &look, language ? "Negro\0Grafito\0\xC3\x8Dndigo\0Plata\0Soul (el del port)\0" : "Black\0Graphite\0Indigo\0Silver\0Soul (the port's own)\0")) {
                settings["border_style"] = std::to_string(look);
                save_settings();
            }
            ImGui::TextWrapped("%s", text("A thicker border leaves a smaller picture: the scale is always a whole number, so every pixel stays square and sharp.",
                                          "Un borde m\xC3\xA1s grueso deja una imagen m\xC3\xA1s peque\xC3\xB1" "a: la escala siempre es un n\xC3\xBAmero entero, as\xC3\xAD cada p\xC3\xADxel queda cuadrado y n\xC3\xADtido."));
        }
        ImGui::BeginDisabled(classic);
        ImGui::TextColored(gold, "%s", text("Picture (enhanced mode)", "Imagen (modo mejorado)"));
        for (int i = 0; i < 5; i++) {
            if (ImGui::RadioButton(language ? labels_es[i] : labels_en[i], view == values[i])) {
                settings["view"] = values[i];
                save_settings();
            }
        }
        ImGui::TextWrapped("%s", text("In field scenes the extra columns show more of the real map; menus and story scenes keep the original picture, centred. Nothing is stretched.",
                                      "En las escenas de campo las columnas extra muestran m\xC3\xA1s del mapa real; los men\xC3\xBAs y escenas de historia conservan la imagen original, centrada. Nada se estira."));
        {
            bool hud = setting("hud_scale", "0") == "1";
            if (ImGui::Checkbox(text("Experimental: larger status display (HP, SP, Soul and the L and R menus)", "Experimental: indicadores m\xC3\xA1s grandes (HP, SP, Soul y los men\xC3\xBAs L y R)"), &hud)) {
                settings["hud_scale"] = hud ? "1" : "0";
                save_settings();
            }
            ImGui::TextWrapped("%s", text("On a large window the field is drawn large and the status display stays as small beside it as on the console. With this on it is drawn with larger pixels, up to twice the field's, as far as it fits the window's width; it keeps its corners and stays sharp. The field, the menus and the messages do not change.",
                                          "En una ventana grande el campo se dibuja grande y los indicadores quedan tan peque\xC3\xB1os a su lado como en la consola. Con esto activado se dibujan con p\xC3\xADxeles m\xC3\xA1s grandes, hasta el doble que los del campo, mientras quepan en el ancho de la ventana; siguen en sus esquinas y n\xC3\xADtidos. El campo, los men\xC3\xBAs y los mensajes no cambian."));
        }
        ImGui::EndDisabled();
        {
            int sync = atoi(setting("vsync", "1").c_str());
            bool skip = setting("skip", "1") != "0";
            ImGui::TextColored(gold, "%s", text("Synchronisation", "Sincronizaci\xC3\xB3n"));
            ImGui::SetNextItemWidth(360);
            if (ImGui::Combo("##vsync", &sync, language ? "Desactivada (puede partir la imagen)\0VSync (recomendado)\0VRR: G-Sync / FreeSync\0"
                                                        : "Off (the picture may tear)\0VSync (recommended)\0VRR: G-Sync / FreeSync\0")) {
                settings["vsync"] = std::to_string(sync);
                save_settings();
            }
            ImGui::TextWrapped("%s", text("VSync: the display paces the game when its refresh rate is 60 Hz or a multiple (120, 180, 240). VRR: for variable-refresh displays in full screen, the game keeps the console's exact 59.73 Hz.",
                                          "VSync: la pantalla marca el ritmo del juego si su frecuencia es 60 Hz o un m\xC3\xBAltiplo (120, 180, 240). VRR: para pantallas de frecuencia variable en pantalla completa, el juego mantiene los 59,73 Hz exactos de la consola."));
            if (ImGui::Checkbox(text("Skip scene with Start (asks first; online everyone has to press it)", "Saltar escena con Start (pregunta antes; en l\xC3\xADnea todos deben pulsarlo)"), &skip)) {
                settings["skip"] = skip ? "1" : "0";
                save_settings();
            }
        }
    } else if (page == 2) {
        int volume = atoi(setting("volume", "100").c_str());
        if (ImGui::SliderInt(text("Volume", "Volumen"), &volume, 0, 100, "%d%%")) {
            settings["volume"] = std::to_string(volume);
            save_settings();
        }
    } else if (page == 3) {
        int count = 0, keys[10], pad[10], defaults[10];
        SDL_JoystickID *pads = SDL_GetGamepads(&count);
        SDL_Gamepad *active = count ? SDL_GetGamepadFromID(pads[0]) : nullptr;
        int profile = controller_profile();
        ImGui::SetNextItemWidth(210);
        if (ImGui::Combo(text("Controller profile", "Perfil de mando"), &profile,
                         language ? "Autom\xC3\xA1tico\0Est\xC3\xA1ndar\0GameCube\0" : "Automatic\0Standard\0GameCube\0")) {
            settings["pad_profile"] = ss_pad_profile_name((ss_pad_profile)profile);
            save_settings();
            capture_pad = -1;
        }
        const bool gamecube = ss_pad_uses_gamecube(active, (ss_pad_profile)profile);
        const char *pad_key = gamecube ? "pad_gamecube" : "pad";
        ss_pad_defaults(active, (ss_pad_profile)profile, defaults);
        bindings("keys", DEFAULT_KEYS, keys);
        bindings(pad_key, defaults, pad, 0);
        if (gamecube)
            ImGui::TextWrapped("%s", text("GameCube: A/B match the cartridge, Z is Select, and L/R use the trigger click.",
                                           "GameCube: A/B como en el cartucho, Z es Select y L/R responden al clic del gatillo."));
        ImGui::TextWrapped("%s", text("Click a box and press the key or the controller button you want for it. Esc cancels. F11 is full screen.",
                                      "Haz clic en una casilla y pulsa la tecla o el bot\xC3\xB3n del mando que quieras para ella. Esc cancela. F11 es pantalla completa."));
        ImGui::TextWrapped("%s", text("While playing, Esc (or Select and Start together on a controller) opens the options menu: screen mode, border, VSync, volume and more can be changed there without leaving the game, and it is where you quit. What you change there is kept here too.",
                                      "Durante el juego, Esc (o Select y Start a la vez en un mando) abre el men\xC3\xBA de opciones: all\xC3\xAD se cambian el modo de pantalla, el borde, VSync, el volumen y m\xC3\xA1s sin salir del juego, y desde \xC3\xA9l se sale. Lo que cambies all\xC3\xAD tambi\xC3\xA9n queda guardado aqu\xC3\xAD."));
        if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn(text("Button", "Bot\xC3\xB3n"), 0, 110);
            ImGui::TableSetupColumn(text("Keyboard", "Teclado"), 0, 190);
            ImGui::TableSetupColumn(text("Controller", "Mando"), 0, 190);
            ImGui::TableHeadersRow();
            for (int i = 0; i < 10; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(language ? BUTTON_NAMES_ES[i] : BUTTON_NAMES[i]);
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                const char *key_name = SDL_GetScancodeName((SDL_Scancode)keys[i]);
                std::string label = capture_key == i ? text("press a key...", "pulsa una tecla...") : (key_name && *key_name ? key_name : "?");
                if (soul_ui::button((label + "##key").c_str(), ImVec2(180, 0))) {
                    capture_key = i;
                    capture_pad = -1;
                }
                ImGui::TableNextColumn();
                /* the name printed on the connected controller (Cross, Circle, A, B...), else SDL's own */
                const char *pad_name = SDL_GetGamepadStringForButton((SDL_GamepadButton)pad[i]);
                if (gamecube && pad[i] == SDL_GAMEPAD_BUTTON_MISC3) pad_name = "L";
                else if (gamecube && pad[i] == SDL_GAMEPAD_BUTTON_MISC4) pad_name = "R";
                else if (gamecube && pad[i] == defaults[2] && pad[i] == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) pad_name = "Z";
                else if (active && pad[i] <= SDL_GAMEPAD_BUTTON_NORTH) {
                    static const char *const face[] = { nullptr, "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle" };
                    static const char *const face_es[] = { nullptr, "A", "B", "X", "Y", "Cruz", "C\xC3\xADrculo", "Cuadrado", "Tri\xC3\xA1ngulo" };
                    int label = (int)SDL_GetGamepadButtonLabel(active, (SDL_GamepadButton)pad[i]);
                    if (label >= 1 && label <= 8)
                        pad_name = language ? face_es[label] : face[label];
                }
                label = capture_pad == i ? text("press a button...", "pulsa un bot\xC3\xB3n...") : (pad_name ? pad_name : "?");
                ImGui::BeginDisabled(count == 0);
                if (soul_ui::button((label + "##pad").c_str(), ImVec2(180, 0))) {
                    capture_pad = i;
                    capture_key = -1;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (soul_ui::button(text("Restore defaults", "Restaurar valores"))) {
            settings.erase("keys");
            settings.erase(pad_key);
            save_settings();
            capture_key = capture_pad = -1;
        }
        ImGui::Spacing();
        if (count == 0)
            ImGui::TextColored(dim, "%s", text("No controller detected. The left stick always works as the d-pad.",
                                               "No se detecta ning\xC3\xBAn mando. El stick izquierdo siempre funciona como cruceta."));
        for (int i = 0; i < count; i++)
            ImGui::BulletText("%s", SDL_GetGamepadNameForID(pads[i]));
        SDL_free(pads);
    } else if (page == 4) {
        bool direct_room = setting("direct_room", "0") == "1";
        char room[32];
        snprintf(room, sizeof room, "%s", setting("room").c_str());
        ImGui::TextColored(gold, "%s", text("Online play (2 to 4 players)", "Juego en l\xC3\xADnea (2 a 4 jugadores)"));
        ImGui::TextWrapped("%s", text("Create or join a room from Multiplayer in the game. Change the server here only if needed.",
                                      "Crea o \xC3\xBA" "nete a una sala desde Multijugador en el juego. Cambia el servidor aqu\xC3\xAD si lo necesitas."));
        if (ImGui::Checkbox(text("Use a room from the launcher", "Usar una sala desde el launcher"), &direct_room)) {
            settings["direct_room"] = direct_room ? "1" : "0";
            save_settings();
        }
        ImGui::BeginDisabled(!direct_room);
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputText(text("Room", "Sala"), room, sizeof room)) {
            settings["room"] = room;
            save_settings();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextColored(gold, "%s", text("Server", "Servidor"));
        {
            std::string how = setting("connection", "official");
            static std::string notice;
            auto choose = [&](const char *value) { settings["connection"] = value; save_settings(); ping_ms = -2; notice.clear(); };
            if (ImGui::RadioButton(text("ShiningSource Server", "Servidor ShiningSource"), how == "official"))
                choose("official");
            if (ImGui::RadioButton(text("Host on this PC", "Alojar en este PC"), how == "host"))
                choose("host");
            if (ImGui::RadioButton(text("Connect by IP", "Conectar por IP"), how == "ip"))
                choose("ip");
            how = setting("connection", "official");
            if (how == "host") {
                bool running = relay_running();
                ImGui::TextWrapped("%s", text("This PC runs the server. Give the others one of these addresses; they choose \"Connect by IP\" and paste it. The server starts when you press Play and ends by itself ten minutes after the last game.",
                                              "Este PC hace de servidor. Pasa a los dem\xC3\xA1s una de estas direcciones; ellos eligen \"Conectar por IP\" y la pegan. El servidor arranca al pulsar Jugar y se cierra solo diez minutos despu\xC3\xA9s de la \xC3\xBAltima partida."));
                if (!running) {
                    if (soul_ui::button(text("Start the server now", "Arrancar el servidor ahora")))
                        notice = start_relay() ? "" : text("The server could not be started (ss_relay.exe must be next to the launcher, and UDP port 47900 free).",
                                                           "No se pudo arrancar el servidor (ss_relay.exe debe estar junto al launcher y el puerto UDP 47900 libre).");
                } else {
                    ImGui::TextColored(green, "%s", text("The server is running.", "El servidor est\xC3\xA1 en marcha."));
                }
                std::vector<std::string> own = local_addresses();
                for (size_t i = 0; i < own.size(); i++) {
                    std::string address = own[i] + ":47900";
                    ImGui::PushID((int)i);
                    ImGui::TextUnformatted(address.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(text("Copy IP", "Copiar IP"))) {
                        SDL_SetClipboardText(address.c_str());
                        notice = text("Copied.", "Copiada.");
                    }
                    ImGui::PopID();
                }
                ImGui::TextWrapped("%s", text("These are this PC's addresses on its own networks. Players outside them reach it through your public address, with UDP port 47900 open towards this PC.",
                                              "Estas son las direcciones de este PC en sus propias redes. Quien est\xC3\xA9 fuera de ellas llega por tu direcci\xC3\xB3n p\xC3\xBA" "blica, con el puerto UDP 47900 abierto hacia este PC."));
            } else if (how == "ip") {
                char address[96];
                snprintf(address, sizeof address, "%s", setting("server_ip").c_str());
                ImGui::SetNextItemWidth(260);
                if (ImGui::InputText(text("Host's address", "Direcci\xC3\xB3n del anfitri\xC3\xB3n"), address, sizeof address)) {
                    settings["server_ip"] = address;
                    save_settings();
                    ping_ms = -2;
                    notice.clear();
                }
                ImGui::SameLine();
                if (soul_ui::button(text("Paste", "Pegar"))) {
                    char *clip = SDL_GetClipboardText();
                    if (clip && *clip) {
                        const std::string pasted = normalized_endpoint(clip);
                        if (pasted.empty()) {
                            notice = text("The clipboard has no valid host address.",
                                          "El portapapeles no contiene una direcci\xC3\xB3n v\xC3\xA1lida.");
                        } else {
                            settings["server_ip"] = pasted;
                            save_settings();
                            ping_ms = -2;
                            notice.clear();
                        }
                    }
                    SDL_free(clip);
                }
                ImGui::TextWrapped("%s", text("The address the host copied from their launcher, for example 192.168.1.20:47900.",
                                              "La direcci\xC3\xB3n que el anfitri\xC3\xB3n copi\xC3\xB3 de su launcher, por ejemplo 192.168.1.20:47900."));
                if (server_address().empty())
                    ImGui::TextColored(red, "%s", text("Enter a host name or IPv4 address with an optional port (1-65535).",
                                                       "Escribe un nombre de host o una direcci\xC3\xB3n IPv4 con puerto opcional (1-65535)."));
            }
            if (!notice.empty())
                ImGui::TextColored(dim, "%s", notice.c_str());
        }
        ImGui::BeginDisabled(server_address().empty());
        if (soul_ui::button(text("Measure my connection", "Medir mi conexi\xC3\xB3n")))
            measure_ping(server_address());
        ImGui::SameLine();
        if (ping_ms == -1)
            ImGui::TextColored(red, "%s", text("The server does not answer.", "El servidor no responde."));
        else if (ping_ms >= 0)
            ImGui::TextColored(green, text("%d ms to the server.", "%d ms hasta el servidor."), ping_ms);
        ImGui::EndDisabled();
    } else if (page == 5) {
        achievements_page(game, gold, dim, red, green);
    } else if (page == 6) {
        qol_page(gold, dim);
    } else if (page == 7) {
        saves_page(gold, dim, red, green);
    } else {
        ImGui::TextColored(gold, "ShiningSource");
        ImGui::TextWrapped("%s", text("The original game logic, compiled from its matching decompilation, running natively. No ROM or game data is included: you supply your own cartridge dump.",
                                      "La l\xC3\xB3gica original del juego, compilada desde su decompilaci\xC3\xB3n exacta, corriendo de forma nativa. No incluye ROM ni datos del juego: t\xC3\xBA aportas el volcado de tu cartucho."));
        ImGui::TextColored(gold, "%s", text("Version 0.9.3", "Versi\xC3\xB3n 0.9.3"));
        ImGui::TextColored(dim, "Dear ImGui %s, SDL %d.%d.%d", IMGUI_VERSION, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
        if (soul_ui::button(text("Exit launcher", "Cerrar launcher"))) running = false;
    }
    content_active = ImGui::IsWindowFocused();
    ImGui::EndChild();
    if (running && games[selected].available && rom_status == "ok" && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        capture_key < 0 && capture_pad < 0 && (start_pressed || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
        play(games[selected]);
    ImGui::End();
}

int wmain(int argc, wchar_t **argv)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof path);
    folder = path;
    folder.erase(folder.find_last_of("\\/") + 1);
    load_settings();
    const char *system_language = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? "es" : "en";
    language = setting("language", system_language) == "es";
    const bool prepare = argc > 1 && !wcscmp(argv[1], L"--prepare");
    if (prepare || (argc > 1 && !wcscmp(argv[1], L"--game"))) {
        int flag = prepare ? 2 : 1;
        if (argc != flag + 2 || wcscmp(argv[flag], L"--game")) {
            fprintf(stderr, "Usage: ShiningSource.exe [--prepare] --game ROM.gba\n");
            return 20;
        }
        if (prepare) {
            puts("progress 0.1 Checking the European ROM");
            fflush(stdout);
        }
        wchar_t rom_path[32768];
        DWORD length = GetFullPathNameW(argv[flag + 1], 32768, rom_path, nullptr);
        if (!length || length >= 32768 || sha1_of(_wfopen(rom_path, L"rb"), games[0].rom_size) != games[0].sha1) {
            fprintf(stderr, "Choose an unmodified Shining Soul II European ROM (AU2P, .gba).\n");
            return 20;
        }
        if (prepare) {
            puts("progress 1.0 European ROM verified");
            return 0;
        }
        int result = play(games[0], "", rom_path);
        if (!launch_error.empty())
            fprintf(stderr, "%s\n", launch_error.c_str());
        return result;
    }
    find_rom_beside(games[0]);
    check_rom(games[0]);

    ss_pad_enable_gamecube();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        MessageBoxA(nullptr, SDL_GetError(), "ShiningSource", MB_ICONERROR);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window *window = SDL_CreateWindow("ShiningSource", 1040, 650, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                                                SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        MessageBoxA(nullptr, SDL_GetError(), "ShiningSource", MB_ICONERROR);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 780, 600);
    SDL_SetWindowHitTest(window, soul_ui::hit_test, nullptr);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    {                                   /* the client takes its folder as UTF-8; a remembered session signs in by itself */
        wchar_t wide[MAX_PATH];
        char utf8[MAX_PATH * 3];
        GetModuleFileNameW(nullptr, wide, MAX_PATH);
        if (wchar_t *slash = wcsrchr(wide, L'\\'))
            *slash = 0;
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, nullptr, nullptr);
        ra = KhRaCreate(utf8, nullptr, nullptr, nullptr);
    }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    soul_ui::style();
    soul_ui::init();
    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool running = true;
    const char *frames = getenv("SS_LAUNCHER_FRAMES");      /* tests: draw N frames and leave */
    long remaining = frames ? atol(frames) : -1;
    LauncherTestPad test_pad(remaining > 0 ? getenv("SS_LAUNCHER_TEST_PAD") : nullptr);
    while (running && remaining != 0 && !launched) {
        test_pad.tick();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool remapping_key = capture_key >= 0;
            ImGui_ImplSDL3_ProcessEvent(&event);
            controller_cancel(event);
            capture_event(event);
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F11 && !remapping_key)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                running = false;
        }
        if (capture_pad >= 0 || captured_button != SDL_GAMEPAD_BUTTON_INVALID)
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        KhRaPump(ra, 0);                 /* answers from the site; nothing is evaluated in the launcher */
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        interface_frame(window, running);
        ImGui::Render();
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.07f, 0.09f, 0.14f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (remaining == 1 && getenv("SS_LAUNCHER_SHOT")) {     /* tests: save the last frame (width, height, RGBA rows bottom-up) */
            std::vector<uint8_t> pixels((size_t)w * h * 4);
            FILE *shot = fopen(getenv("SS_LAUNCHER_SHOT"), "wb");
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (shot) {
                fwrite(&w, 4, 1, shot);
                fwrite(&h, 4, 1, shot);
                fwrite(pixels.data(), 1, pixels.size(), shot);
                fclose(shot);
            }
        }
        SDL_GL_SwapWindow(window);
        if (remaining > 0)
            remaining--;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    test_pad.close();
    ImGui::DestroyContext();
    glDeleteTextures(1, &soul_ui::grain);
    glDeleteTextures(1, &soul_ui::cover);
    if (title_texture) glDeleteTextures(1, &title_texture);
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
