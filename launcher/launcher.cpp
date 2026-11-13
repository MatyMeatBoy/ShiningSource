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
 * automated tests drive it, so there is one mechanism to keep working.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "achievements/ra_client.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl3.h"

/* ---- games ---- */

struct Game {
    const char *id, *title, *region, *sha1, *host;
    bool available;
};
static const Game games[] = {
    { "ss2", "Shining Soul II", "Europe (AU2P)", "aa1288be9257337abdfe3034898cf6c7aa778fbc", "ss2_win64.exe", true },
    { "ss1", "Shining Soul", "", "", "", false },
};

/* ---- language ---- */

static int language;     /* 0 English, 1 Spanish */
static const char *text(const char *english, const char *spanish) { return language ? spanish : english; }

/* ---- SHA-1 of a file ---- */

static std::string sha1_of(const std::string &path)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return "";
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    std::vector<uint8_t> data;
    uint8_t buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, file)) > 0)
        data.insert(data.end(), buffer, buffer + got);
    fclose(file);
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
        *equals = 0;
        std::string value = equals + 1;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
            value.pop_back();
        settings[line] = value;
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
    std::string digest = sha1_of(path);
    rom_status = digest.empty() ? "missing" : digest == game.sha1 ? "ok" : "wrong";
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

static void choose_rom(const Game &game)
{
    char path[MAX_PATH] = "";
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof dialog;
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
static void measure_ping(const std::string &server)
{
    WSADATA data;
    std::string host = server.substr(0, server.find(':'));
    std::string port = server.find(':') == std::string::npos ? "47900" : server.substr(server.find(':') + 1);
    addrinfo hints = {}, *found = nullptr;
    ping_ms = -1;
    WSAStartup(MAKEWORD(2, 2), &data);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) || !found)
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
static const int DEFAULT_PAD[10] = { SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
                                     SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP,
                                     SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER };
static int capture_key = -1, capture_pad = -1;      /* the entry waiting for a press, or -1 */

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
        if (event.key.scancode != SDL_SCANCODE_ESCAPE) {        /* Esc cancels, and always quits the game */
            bindings("keys", DEFAULT_KEYS, values);
            values[capture_key] = (int)event.key.scancode;
            store_bindings("keys", values);
        }
        capture_key = -1;
    }
    if (capture_pad >= 0 && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        bindings("pad", DEFAULT_PAD, values, 0);
        values[capture_pad] = (int)event.gbutton.button;
        store_bindings("pad", values);
        capture_pad = -1;
    }
    if (event.type == SDL_EVENT_GAMEPAD_ADDED)
        SDL_OpenGamepad(event.gdevice.which);       /* an open controller is what sends button events */
}

/* ---- RetroAchievements ----
 * The user signs in with their own account; the client (launcher/achievements, on rcheevos) asks the site for the
 * achievement list of the ROM they chose and shows it. The password goes to retroachievements.org and nowhere
 * else, and is not kept: what stays on this PC is the session token the site returns, protected for this Windows
 * user. Nothing is evaluated here: the game does that (see docs/ROADMAP.md for what is and is not sent). */
static KhRa *ra;
static std::string ra_rom;          /* the ROM the list was asked for */

static void achievements_page(const Game &game, const ImVec4 &gold, const ImVec4 &dim, const ImVec4 &red, const ImVec4 &green)
{
    static char username[64], password[128];
    static bool remember = true;
    rc_client_t *client = KhRaClient(ra);
    const rc_client_user_t *user = client ? rc_client_get_user_info(client) : nullptr;
    std::string rom = setting((std::string("rom_") + game.id).c_str());

    ImGui::TextColored(gold, "%s", text("Achievements (RetroAchievements)", "Logros (RetroAchievements)"));
    if (!ra) {
        ImGui::TextColored(red, "%s", text("The achievements client could not be started.", "No se pudo iniciar el cliente de logros."));
        return;
    }
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
    if (user) {
        ImGui::TextColored(green, text("Signed in as %s", "Sesi\xC3\xB3n iniciada como %s"), user->display_name);
        ImGui::SameLine();
        if (ImGui::Button(text("Sign out", "Cerrar sesi\xC3\xB3n")))
            KhRaLogout(ra);
    } else {
        ImGui::TextWrapped("%s", text("Sign in with your RetroAchievements account to see this game's achievements.",
                                      "Inicia sesi\xC3\xB3n con tu cuenta de RetroAchievements para ver los logros de este juego."));
        ImGui::SetNextItemWidth(260);
        ImGui::InputText(text("User", "Usuario"), username, sizeof username);
        ImGui::SetNextItemWidth(260);
        ImGui::InputText(text("Password", "Contrase\xC3\xB1" "a"), password, sizeof password, ImGuiInputTextFlags_Password);
        ImGui::Checkbox(text("Keep me signed in on this PC", "Mantener la sesi\xC3\xB3n en este PC"), &remember);
        ImGui::BeginDisabled(!username[0] || !password[0] || KhRaBusy(ra));
        if (ImGui::Button(text("Sign in", "Iniciar sesi\xC3\xB3n"))) {
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
                total++;
                points += a->points;
                unlocked += a->unlocked != 0;
            }
        ImGui::Text(text("%u of %u unlocked on your account, %u points in the set", "%u de %u desbloqueados en tu cuenta, %u puntos en el conjunto"), unlocked, total, points);
        ImGui::TextColored(dim, "%s", text("Normal list: earned by playing without cheats. Rebel mode (sessions with cheats) keeps its own record and never touches this one.",
                                           "Lista normal: se gana jugando sin trucos. El modo rebelde (sesiones con trucos) lleva su propio registro y nunca toca este."));
        ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (uint32_t b = 0; b < list->num_buckets; b++)
            for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++) {
                const rc_client_achievement_t *a = list->buckets[b].achievements[i];
                ImGui::TextColored(a->unlocked ? gold : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s %s  (%u)", a->unlocked ? "*" : "-", a->title, a->points);
                ImGui::TextColored(dim, "    %s", a->description);
            }
        ImGui::EndChild();
    }
    if (list)
        rc_client_destroy_achievement_list(list);
}

/* ---- start the game ---- */

static std::string launch_error;

static void play(const Game &game)
{
    std::string host = folder + game.host, rom = setting((std::string("rom_") + game.id).c_str());
    std::string command = "\"" + host + "\" \"" + rom + "\"";
    STARTUPINFOA startup = {};
    PROCESS_INFORMATION process = {};
    std::string view = setting("view", "auto");

    bool classic = setting("mode", "enhanced") == "classic";
    SetEnvironmentVariableA("SS_MODE", classic ? "classic" : nullptr);
    SetEnvironmentVariableA("SS_BORDER", classic ? setting("border", "16").c_str() : nullptr);
    SetEnvironmentVariableA("SS_BORDER_STYLE", classic ? setting("border_style", "4").c_str() : nullptr);
    SetEnvironmentVariableA("SS_VIEW", view == "auto" || classic ? nullptr : view.c_str());
    SetEnvironmentVariableA("SS_VSYNC", setting("vsync", "1").c_str());
    SetEnvironmentVariableA("SS_LANG", language ? "es" : "en");
    SetEnvironmentVariableA("SS_SKIP", setting("skip", "1") == "0" ? "0" : nullptr);
    SetEnvironmentVariableA("SS_FULLSCREEN", setting("fullscreen", "0") == "1" ? "1" : nullptr);
    SetEnvironmentVariableA("SS_VOLUME", setting("volume", "100").c_str());
    SetEnvironmentVariableA("SS_KEYS", setting("keys").empty() ? nullptr : setting("keys").c_str());
    SetEnvironmentVariableA("SS_PAD", setting("pad").empty() ? nullptr : setting("pad").c_str());
    bool online = setting("online", "0") == "1";
    SetEnvironmentVariableA("SS_NET", online ? setting("server", DEFAULT_SERVER).c_str() : nullptr);
    SetEnvironmentVariableA("SS_NET_ROOM", online ? setting("room", "shining").c_str() : nullptr);
    SetEnvironmentVariableA("SS_NET_DELAY", online && setting("delay_auto", "1") == "0" ? setting("delay", "6").c_str() : nullptr);
    startup.cb = sizeof startup;
    launch_error.clear();
    if (!CreateProcessA(host.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process)) {
        launch_error = text("The game program could not be started: ", "No se pudo iniciar el programa del juego: ") + host;
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

/* ---- interface ---- */

/* The launcher wears the port's own "Soul" look, the same as the classic mode's border: a deep blue-green field
 * with points of light, gold frames and jewels, in the spirit of the game's menus. Everything is drawn here; no
 * artwork of the game is in the program. */
static void style()
{
    ImGuiStyle &s = ImGui::GetStyle();
    const ImVec4 gold(0.91f, 0.66f, 0.22f, 1), pale(1.0f, 0.89f, 0.54f, 1), teal(0.31f, 0.55f, 0.59f, 1);
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 2;
    s.GrabRounding = 2;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 2;
    s.FrameBorderSize = 1;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.WindowPadding = ImVec2(18, 14);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);                   /* the field is drawn behind everything */
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.14f, 0.18f, 0.88f);
    s.Colors[ImGuiCol_Border] = gold;
    s.Colors[ImGuiCol_Text] = ImVec4(0.96f, 0.96f, 0.92f, 1);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.66f, 0.68f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.40f, 0.45f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = teal;
    s.Colors[ImGuiCol_ButtonActive] = gold;
    s.Colors[ImGuiCol_Header] = ImVec4(0.72f, 0.48f, 0.12f, 1);        /* the selected entry: gold, as the game's cursor */
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.48f, 0.52f, 1);
    s.Colors[ImGuiCol_HeaderActive] = gold;
    s.Colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.22f, 0.27f, 1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.30f, 0.35f, 1);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.36f, 0.41f, 1);
    s.Colors[ImGuiCol_CheckMark] = pale;
    s.Colors[ImGuiCol_SliderGrab] = gold;
    s.Colors[ImGuiCol_SliderGrabActive] = pale;
    s.Colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.14f, 0.18f, 0.98f);
    s.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.26f, 0.31f, 1);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.25f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.72f, 0.48f, 0.12f, 1);
    s.Colors[ImGuiCol_Separator] = gold;
}

static void jewel(ImDrawList *draw, ImVec2 centre, float size, ImU32 colour)
{
    draw->AddQuadFilled(ImVec2(centre.x, centre.y - size), ImVec2(centre.x + size, centre.y), ImVec2(centre.x, centre.y + size),
                        ImVec2(centre.x - size, centre.y), colour);
    draw->AddLine(ImVec2(centre.x - size * 0.45f, centre.y - size * 0.35f), ImVec2(centre.x, centre.y - size * 0.8f), IM_COL32(255, 250, 220, 255), 1.5f);
}

/* The field behind the whole window, and the frame around it. */
static void backdrop()
{
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const ImU32 gold = IM_COL32(232, 168, 56, 255), cyan = IM_COL32(92, 214, 226, 255);
    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(20, 62, 74, 255), IM_COL32(20, 62, 74, 255), IM_COL32(8, 24, 34, 255), IM_COL32(8, 24, 34, 255));
    for (int y = 9; y < (int)size.y; y += 21)
        for (int x = 9 + (y * 5) % 33; x < (int)size.x; x += 39) {
            int k = (x * 31 + y * 17) % 5;
            draw->AddRectFilled(ImVec2((float)x, (float)y), ImVec2((float)x + 2, (float)y + 2), IM_COL32(60 + k * 22, 120 + k * 20, 130 + k * 20, 255));
        }
    draw->AddRect(ImVec2(5, 5), ImVec2(size.x - 5, size.y - 5), IM_COL32(96, 58, 16, 255), 0, 0, 5);
    draw->AddRect(ImVec2(5, 5), ImVec2(size.x - 5, size.y - 5), gold, 0, 0, 3);
    draw->AddRect(ImVec2(6, 6), ImVec2(size.x - 6, size.y - 6), IM_COL32(255, 226, 138, 255), 0, 0, 1);
    jewel(draw, ImVec2(6, 6), 9, gold); jewel(draw, ImVec2(size.x - 6, 6), 9, gold);
    jewel(draw, ImVec2(6, size.y - 6), 9, gold); jewel(draw, ImVec2(size.x - 6, size.y - 6), 9, gold);
    jewel(draw, ImVec2(size.x / 2, 6), 8, cyan); jewel(draw, ImVec2(size.x / 2, size.y - 6), 8, cyan);
}

static void interface_frame(bool &running)
{
    static int selected = 0, page = 0;
    static bool first = true;
    if (first && getenv("SS_LAUNCHER_PAGE"))      /* tests: open on a given page */
        page = atoi(getenv("SS_LAUNCHER_PAGE"));
    first = false;
    const Game &game = games[selected];
    const ImVec4 gold(0.84f, 0.66f, 0.29f, 1), red(0.95f, 0.45f, 0.40f, 1), green(0.45f, 0.85f, 0.50f, 1), dim(0.6f, 0.65f, 0.75f, 1);

    backdrop();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("ShiningSource", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(gold, "ShiningSource");
    ImGui::SameLine();
    ImGui::TextColored(dim, "%s", text("native port of the Shining Soul games", "port nativo de los juegos Shining Soul"));
    ImGui::SameLine(ImGui::GetWindowWidth() - 150);
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##language", &language, "English\0Espa\xC3\xB1ol\0")) {
        settings["language"] = language ? "es" : "en";
        save_settings();
    }
    ImGui::Separator();

    /* left: games and pages */
    ImGui::BeginChild("left", ImVec2(210, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(dim, "%s", text("GAMES", "JUEGOS"));
    for (int i = 0; i < (int)(sizeof games / sizeof games[0]); i++) {
        std::string label = std::string(games[i].title) + (games[i].available ? "" : text("  (closed)", "  (cerrado)"));
        if (ImGui::Selectable(label.c_str(), selected == i)) {
            selected = i;
            check_rom(games[i]);
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(dim, "%s", text("PAGES", "SECCIONES"));
    const char *pages_en[] = { "Play", "Video", "Sound", "Controls", "Online", "Achievements", "About" };
    const char *pages_es[] = { "Jugar", "V\xC3\xAD" "deo", "Sonido", "Controles", "En l\xC3\xADnea", "Logros", "Acerca de" };
    for (int i = 0; i < 7; i++)
        if (ImGui::Selectable(language ? pages_es[i] : pages_en[i], page == i))
            page = i;
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (!game.available) {
        ImGui::TextColored(gold, "%s", game.title);
        ImGui::TextWrapped("%s", text("This game is closed: it has no decompilation yet, so there is nothing to build. It will open here when its decompilation exists.",
                                      "Este juego est\xC3\xA1 cerrado: a\xC3\xBAn no tiene decompilaci\xC3\xB3n, as\xC3\xAD que no hay nada que construir. Se abrir\xC3\xA1 aqu\xC3\xAD cuando exista."));
    } else if (page == 0) {
        std::string rom = setting((std::string("rom_") + game.id).c_str());
        if (rom_status == "ok" || rom_status == "wrong")
            make_title(game);
        if (title_texture && rom_status != "missing" && !rom.empty()) {
            /* the game's title screen, from the user's own ROM, in a gold frame */
            /* as large as leaves room for the page below it */
            ImVec2 room = ImGui::GetContentRegionAvail();
            float scale = room.x >= 740 && room.y >= 840 ? 3.0f : room.x >= 500 && room.y >= 660 ? 2.0f : 1.0f;
            ImVec2 at = ImGui::GetCursorScreenPos(), size(240 * scale, 160 * scale);
            at.x += (ImGui::GetContentRegionAvail().x - size.x) / 2;
            ImDrawList *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(ImVec2(at.x - 5, at.y - 1), ImVec2(at.x + size.x + 5, at.y + size.y + 9), IM_COL32(96, 58, 16, 255));
            draw->AddRectFilled(ImVec2(at.x - 4, at.y), ImVec2(at.x + size.x + 4, at.y + size.y + 8), IM_COL32(232, 168, 56, 255));
            draw->AddRectFilled(ImVec2(at.x - 2, at.y + 2), ImVec2(at.x + size.x + 2, at.y + size.y + 6), IM_COL32(255, 226, 138, 255));
            draw->AddImage((ImTextureID)(intptr_t)title_texture, ImVec2(at.x, at.y + 4), ImVec2(at.x + size.x, at.y + size.y + 4));
            ImGui::Dummy(ImVec2(0, size.y + 16));
        }
        ImGui::TextColored(gold, "%s", game.title);
        ImGui::TextColored(dim, "%s", game.region);
        ImGui::Spacing();
        ImGui::TextUnformatted(text("Your cartridge dump (the game is not included):", "Tu volcado del cartucho (el juego no viene incluido):"));
        ImGui::TextWrapped("%s", rom.empty() ? text("(none chosen)", "(ninguno elegido)") : rom.c_str());
        if (rom_status == "ok")
            ImGui::TextColored(green, "%s", text("Verified: this is the supported version.", "Verificado: es la versi\xC3\xB3n compatible."));
        else if (rom_status == "wrong")
            ImGui::TextColored(red, "%s", text("This file is not the supported version (its SHA-1 differs). The game may not work.",
                                               "Este archivo no es la versi\xC3\xB3n compatible (su SHA-1 es distinto). Puede que el juego no funcione."));
        else if (rom_status == "missing")
            ImGui::TextColored(red, "%s", text("The file is no longer there.", "El archivo ya no est\xC3\xA1."));
        if (ImGui::Button(text("Choose ROM...", "Elegir ROM...")))
            choose_rom(game);
        ImGui::Spacing();
        if (setting("mode").empty()) {
            /* first start: the two ways to see the game */
            ImGui::TextColored(gold, "%s", text("How do you want to see the game?", "\xC2\xBF" "C\xC3\xB3mo quieres ver el juego?"));
            if (ImGui::Button(text("Classic", "Cl\xC3\xA1sico"), ImVec2(220, 54))) {
                settings["mode"] = "classic";
                save_settings();
            }
            ImGui::SameLine();
            if (ImGui::Button(text("Enhanced", "Mejorado"), ImVec2(220, 54))) {
                settings["mode"] = "enhanced";
                save_settings();
            }
            ImGui::TextWrapped("%s", text("Classic: the console's picture, as an emulator shows it, at a whole scale inside a border you can adjust. Enhanced: the picture adapts to the window and shows more of the map. You can change it later in Video.",
                                          "Cl\xC3\xA1sico: la imagen de la consola, como la muestra un emulador, a escala entera dentro de un borde que puedes ajustar. Mejorado: la imagen se adapta a la ventana y muestra m\xC3\xA1s mapa. Puedes cambiarlo luego en V\xC3\xAD" "deo."));
        } else {
            ImGui::BeginDisabled(rom.empty() || rom_status == "missing");
            if (ImGui::Button(text("PLAY", "JUGAR"), ImVec2(220, 54)))
                play(game);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(dim, "%s", setting("mode") == "classic" ? text("Classic mode", "Modo cl\xC3\xA1sico") : text("Enhanced mode", "Modo mejorado"));
        }
        if (!launch_error.empty())
            ImGui::TextColored(red, "%s", launch_error.c_str());
        ImGui::Spacing();
        ImGui::TextColored(dim, "SHA-1 %s", game.sha1);
    } else if (page == 1) {
        static const char *values[] = { "auto", "240", "284", "320", "480" };
        const char *labels_en[] = { "Automatic: fill the window", "Original 3:2 (240 columns)", "16:9 (284 columns)", "Wider (320 columns)", "Maximum (480 columns)" };
        const char *labels_es[] = { "Autom\xC3\xA1tico: llenar la ventana", "Original 3:2 (240 columnas)", "16:9 (284 columnas)", "M\xC3\xA1s ancho (320 columnas)", "M\xC3\xA1ximo (480 columnas)" };
        std::string view = setting("view", "auto");
        bool fullscreen = setting("fullscreen", "0") == "1";
        bool classic = setting("mode", "enhanced") == "classic";
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
        if (ImGui::Checkbox(text("Start in full screen (F11 switches while playing)", "Empezar en pantalla completa (F11 cambia durante el juego)"), &fullscreen)) {
            settings["fullscreen"] = fullscreen ? "1" : "0";
            save_settings();
        }
    } else if (page == 2) {
        int volume = atoi(setting("volume", "100").c_str());
        ImGui::TextColored(gold, "%s", text("Sound", "Sonido"));
        if (ImGui::SliderInt(text("Volume", "Volumen"), &volume, 0, 100, "%d%%")) {
            settings["volume"] = std::to_string(volume);
            save_settings();
        }
    } else if (page == 3) {
        int count = 0, keys[10], pad[10];
        SDL_JoystickID *pads = SDL_GetGamepads(&count);
        bindings("keys", DEFAULT_KEYS, keys);
        bindings("pad", DEFAULT_PAD, pad, 0);
        ImGui::TextColored(gold, "%s", text("Controls", "Controles"));
        ImGui::TextWrapped("%s", text("Click a box and press the key or the controller button you want for it. Esc cancels. F11 is full screen and Esc quits the game.",
                                      "Haz clic en una casilla y pulsa la tecla o el bot\xC3\xB3n del mando que quieras para ella. Esc cancela. F11 es pantalla completa y Esc sale del juego."));
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
                if (ImGui::Button((label + "##key").c_str(), ImVec2(180, 0))) {
                    capture_key = i;
                    capture_pad = -1;
                }
                ImGui::TableNextColumn();
                /* the name printed on the connected controller (Cross, Circle, A, B...), else SDL's own */
                const char *pad_name = SDL_GetGamepadStringForButton((SDL_GamepadButton)pad[i]);
                if (count > 0 && pad[i] <= SDL_GAMEPAD_BUTTON_NORTH) {
                    static const char *const face[] = { nullptr, "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle" };
                    static const char *const face_es[] = { nullptr, "A", "B", "X", "Y", "Cruz", "C\xC3\xADrculo", "Cuadrado", "Tri\xC3\xA1ngulo" };
                    int label = (int)SDL_GetGamepadButtonLabelForType(SDL_GetGamepadTypeForID(pads[0]), (SDL_GamepadButton)pad[i]);
                    if (label >= 1 && label <= 8)
                        pad_name = language ? face_es[label] : face[label];
                }
                label = capture_pad == i ? text("press a button...", "pulsa un bot\xC3\xB3n...") : (pad_name ? pad_name : "?");
                ImGui::BeginDisabled(count == 0);
                if (ImGui::Button((label + "##pad").c_str(), ImVec2(180, 0))) {
                    capture_pad = i;
                    capture_key = -1;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(text("Restore defaults", "Restaurar valores"))) {
            settings.erase("keys");
            settings.erase("pad");
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
        bool online = setting("online", "0") == "1";
        int delay = atoi(setting("delay", "6").c_str());
        char room[32], server[96];
        snprintf(room, sizeof room, "%s", setting("room", "shining").c_str());
        snprintf(server, sizeof server, "%s", setting("server", DEFAULT_SERVER).c_str());
        ImGui::TextColored(gold, "%s", text("Online play (2 to 4 players)", "Juego en l\xC3\xADnea (2 a 4 jugadores)"));
        if (ImGui::Checkbox(text("Play online", "Jugar en l\xC3\xADnea"), &online)) {
            settings["online"] = online ? "1" : "0";
            save_settings();
        }
        ImGui::BeginDisabled(!online);
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputText(text("Room", "Sala"), room, sizeof room)) {
            settings["room"] = room;
            save_settings();
        }
        ImGui::TextWrapped("%s", text("Everyone types the same room name. The first to enter is the host (player 1). In the game choose MULTIPLAYER MODE: the cable is the network.",
                                      "Todos escriben el mismo nombre de sala. El primero en entrar es el anfitri\xC3\xB3n (jugador 1). En el juego elige MULTIPLAYER MODE: el cable es la red."));
        bool automatic = setting("delay_auto", "1") != "0";
        if (ImGui::Checkbox(text("Automatic delay (recommended): as short as the connection allows", "Retardo autom\xC3\xA1tico (recomendado): tan corto como permita la conexi\xC3\xB3n"), &automatic)) {
            settings["delay_auto"] = automatic ? "1" : "0";
            save_settings();
        }
        ImGui::BeginDisabled(automatic);
        ImGui::SetNextItemWidth(260);
        if (ImGui::SliderInt(text("Delay (host decides)", "Retardo (lo decide el anfitri\xC3\xB3n)"), &delay, 2, 30, text("%d frames", "%d cuadros"))) {
            settings["delay"] = std::to_string(delay);
            save_settings();
        }
        ImGui::TextWrapped("%s", text("Same country: 3 or 4. Same continent: 6. Between continents: 10 to 14. Too low and the game slows down, it never disconnects; too high and the other players react later.",
                                      "Mismo pa\xC3\xADs: 3 o 4. Mismo continente: 6. Entre continentes: 10 a 14. Si es muy bajo el juego va m\xC3\xA1s lento, nunca se desconecta; si es muy alto los dem\xC3\xA1s reaccionan m\xC3\xA1s tarde."));
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputText(text("Server", "Servidor"), server, sizeof server)) {
            settings["server"] = server;
            save_settings();
            ping_ms = -2;
        }
        if (ImGui::Button(text("Measure my connection", "Medir mi conexi\xC3\xB3n")))
            measure_ping(server);
        ImGui::SameLine();
        if (ping_ms == -1)
            ImGui::TextColored(red, "%s", text("The server does not answer.", "El servidor no responde."));
        else if (ping_ms >= 0) {
            int suggested = (ping_ms + 40 + 16) / 17;
            ImGui::TextColored(green, text("%d ms. If the others measure about the same, delay %d.", "%d ms. Si los dem\xC3\xA1s miden parecido, retardo %d."),
                               ping_ms, suggested < 2 ? 2 : suggested > 30 ? 30 : suggested);
        }
        ImGui::EndDisabled();
    } else if (page == 5) {
        achievements_page(game, gold, dim, red, green);
    } else {
        ImGui::TextColored(gold, "ShiningSource");
        ImGui::TextWrapped("%s", text("The original game logic, compiled from its matching decompilation, running natively. No ROM or game data is included: you supply your own cartridge dump.",
                                      "La l\xC3\xB3gica original del juego, compilada desde su decompilaci\xC3\xB3n exacta, corriendo de forma nativa. No incluye ROM ni datos del juego: t\xC3\xBA aportas el volcado de tu cartucho."));
        ImGui::TextColored(gold, "%s", text("Version 0.1.1", "Versión 0.1.1"));
        ImGui::TextColored(dim, "Dear ImGui %s, SDL %d.%d.%d", IMGUI_VERSION, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    }
    ImGui::EndChild();
    ImGui::End();
    (void)running;
}

int main(int, char **)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof path);
    folder = path;
    folder.erase(folder.find_last_of("\\/") + 1);
    load_settings();
    language = setting("language", "en") == "es";
    check_rom(games[0]);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        MessageBoxA(nullptr, SDL_GetError(), "ShiningSource", MB_ICONERROR);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window *window = SDL_CreateWindow("ShiningSource", 880, 540, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        MessageBoxA(nullptr, SDL_GetError(), "ShiningSource", MB_ICONERROR);
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    ra = KhRaCreate(folder.c_str(), nullptr, nullptr, nullptr);     /* a remembered session signs in by itself */
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().FontGlobalScale = 1.25f;
    style();
    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool running = true;
    const char *frames = getenv("SS_LAUNCHER_FRAMES");      /* tests: draw N frames and leave */
    long remaining = frames ? atol(frames) : -1;
    while (running && remaining != 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            capture_event(event);
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F11 && capture_key < 0)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }
        KhRaPump(ra, 0);                 /* answers from the site; nothing is evaluated in the launcher */
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        interface_frame(running);
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
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
