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
 * Settings reach the game as environment variables (SS_VIEW, SS_FULLSCREEN, SS_VOLUME), which is also how the
 * automated tests drive it, so there is one mechanism to keep working.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

/* ---- start the game ---- */

static std::string launch_error;

static void play(const Game &game)
{
    std::string host = folder + game.host, rom = setting((std::string("rom_") + game.id).c_str());
    std::string command = "\"" + host + "\" \"" + rom + "\"";
    STARTUPINFOA startup = {};
    PROCESS_INFORMATION process = {};
    std::string view = setting("view", "auto");

    SetEnvironmentVariableA("SS_VIEW", view == "auto" ? nullptr : view.c_str());
    SetEnvironmentVariableA("SS_FULLSCREEN", setting("fullscreen", "0") == "1" ? "1" : nullptr);
    SetEnvironmentVariableA("SS_VOLUME", setting("volume", "100").c_str());
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

static void style()
{
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(10, 10);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.09f, 0.14f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.30f, 0.55f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.40f, 0.70f, 1);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.84f, 0.66f, 0.29f, 1);
    s.Colors[ImGuiCol_Header] = ImVec4(0.16f, 0.30f, 0.55f, 1);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.40f, 0.70f, 1);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.16f, 0.24f, 1);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(0.84f, 0.66f, 0.29f, 1);
    s.Colors[ImGuiCol_SliderGrab] = ImVec4(0.84f, 0.66f, 0.29f, 1);
}

static void interface_frame(bool &running)
{
    static int selected = 0, page = 0;
    const Game &game = games[selected];
    const ImVec4 gold(0.84f, 0.66f, 0.29f, 1), red(0.95f, 0.45f, 0.40f, 1), green(0.45f, 0.85f, 0.50f, 1), dim(0.6f, 0.65f, 0.75f, 1);

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
    const char *pages_en[] = { "Play", "Video", "Sound", "Controls", "About" };
    const char *pages_es[] = { "Jugar", "V\xC3\xAD" "deo", "Sonido", "Controles", "Acerca de" };
    for (int i = 0; i < 5; i++)
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
        ImGui::BeginDisabled(rom.empty() || rom_status == "missing");
        if (ImGui::Button(text("PLAY", "JUGAR"), ImVec2(220, 54)))
            play(game);
        ImGui::EndDisabled();
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
        ImGui::TextColored(gold, "%s", text("Picture", "Imagen"));
        for (int i = 0; i < 5; i++) {
            if (ImGui::RadioButton(language ? labels_es[i] : labels_en[i], view == values[i])) {
                settings["view"] = values[i];
                save_settings();
            }
        }
        ImGui::TextWrapped("%s", text("In field scenes the extra columns show more of the real map; menus and story scenes keep the original picture, centred. Nothing is stretched.",
                                      "En las escenas de campo las columnas extra muestran m\xC3\xA1s del mapa real; los men\xC3\xBAs y escenas de historia conservan la imagen original, centrada. Nada se estira."));
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
        int count = 0;
        SDL_JoystickID *pads = SDL_GetGamepads(&count);
        ImGui::TextColored(gold, "%s", text("Controls", "Controles"));
        ImGui::TextUnformatted(text("Keyboard: arrows, X = A, Z = B, C = L, V = R, Enter = Start, right Shift = Select, F11 full screen, Esc quit.",
                                    "Teclado: flechas, X = A, Z = B, C = L, V = R, Enter = Start, Shift derecho = Select, F11 pantalla completa, Esc salir."));
        ImGui::Spacing();
        if (count == 0)
            ImGui::TextColored(dim, "%s", text("No controller detected.", "No se detecta ning\xC3\xBAn mando."));
        for (int i = 0; i < count; i++)
            ImGui::BulletText("%s", SDL_GetGamepadNameForID(pads[i]));
        if (count)
            ImGui::TextUnformatted(text("Controller: d-pad or left stick, A and B as on the console, shoulders = L and R, Start, Back = Select.",
                                        "Mando: cruceta o stick izquierdo, A y B como en la consola, gatillos = L y R, Start, Back = Select."));
        SDL_free(pads);
    } else {
        ImGui::TextColored(gold, "ShiningSource");
        ImGui::TextWrapped("%s", text("The original game logic, compiled from its matching decompilation, running natively. No ROM or game data is included: you supply your own cartridge dump.",
                                      "La l\xC3\xB3gica original del juego, compilada desde su decompilaci\xC3\xB3n exacta, corriendo de forma nativa. No incluye ROM ni datos del juego: t\xC3\xBA aportas el volcado de tu cartucho."));
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
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }
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
