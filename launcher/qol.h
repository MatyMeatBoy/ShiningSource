/* The "Quality of life" page of the launcher: additions of the port the original game does not have, each
 * one a setting that is off until the player ticks it. Part of launcher.cpp (included once, after the settings
 * and text() exist); kept in its own file so the page can grow without touching the rest.
 *
 * Experimental: "L and R bars: LT / RT step backwards" (setting qol_bars, SS_QOL_BARS=1 for the game) and the
 * two keyboard keys of the same actions (setting keys_qol, SS_KEYS_QOL: two SDL scancodes). See
 * runtime/core/qol.c for what the game does with them. */

static const int QOL_DEFAULT_KEYS[2] = { SDL_SCANCODE_D, SDL_SCANCODE_F };
static int qol_capture = -1;        /* the key box waiting for a press, or -1 */

static void qol_keys(int *out)
{
    std::string list = setting("keys_qol");
    const char *at = list.c_str();
    for (int i = 0; i < 2; i++) {
        char *end;
        long value = strtol(at, &end, 10);
        out[i] = end != at && value >= 1 && value < SDL_SCANCODE_COUNT ? (int)value : QOL_DEFAULT_KEYS[i];
        at = *end == ',' ? end + 1 : end;
    }
}

/* What play() hands to the game. */
static void qol_environment()
{
    SetEnvironmentVariableA("SS_QOL_BARS", setting("qol_bars", "0") == "1" ? "1" : nullptr);
    SetEnvironmentVariableA("SS_KEYS_QOL", setting("keys_qol").empty() ? nullptr : setting("keys_qol").c_str());
}

static void qol_page(const ImVec4 &gold, const ImVec4 &dim)
{
    bool bars = setting("qol_bars", "0") == "1";
    int keys[2];
    qol_keys(keys);

    ImGui::TextColored(gold, "%s", text("Quality of life", "Calidad de vida"));
    ImGui::TextWrapped("%s", text("Additions of the port that the original game does not have. Everything here is off until you tick it; with all of it off the game plays exactly as on the cartridge.",
                                  "A\xC3\xB1" "adidos del port que el juego original no tiene. Todo est\xC3\xA1 desactivado hasta que lo marques; con todo desactivado el juego se juega exactamente como en el cartucho."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(gold, "%s", text("Experimental", "Experimental"));
    ImGui::TextColored(dim, "%s", text("New and little tested. Tell us how they feel.", "Nuevas y poco probadas. Cu\xC3\xA9ntanos qu\xC3\xA9 tal van."));
    ImGui::Spacing();
    if (ImGui::Checkbox(text("L and R bars: LT / RT step backwards", "Barras L y R: LT / RT retroceden"), &bars)) {
        settings["qol_bars"] = bars ? "1" : "0";
        save_settings();
    }
    ImGui::TextWrapped("%s", text("In the game L and R only move the quick bars (weapons at the left, items at the right) to the next slot. With this on, the controller's left trigger (LT, L2) moves the L bar one slot back and the right trigger (RT, R2) the R bar; L and R still move them forwards. Online it is enough that you have it on: the other players see the same result.",
                                  "En el juego L y R solo mueven las barras r\xC3\xA1pidas (armas a la izquierda, objetos a la derecha) a la casilla siguiente. Con esto activado, el gatillo izquierdo del mando (LT, L2) mueve la barra L una casilla hacia atr\xC3\xA1s y el gatillo derecho (RT, R2) la barra R; L y R siguen movi\xC3\xA9ndolas hacia delante. En l\xC3\xADnea basta con que lo tengas t\xC3\xBA: los dem\xC3\xA1s jugadores ven el mismo resultado."));
    ImGui::Spacing();
    ImGui::BeginDisabled(!bars);
    if (ImGui::BeginTable("qol keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(text("Action", "Acci\xC3\xB3n"), 0, 230);
        ImGui::TableSetupColumn(text("Keyboard", "Teclado"), 0, 190);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 2; i++) {
            const char *name = SDL_GetScancodeName((SDL_Scancode)keys[i]);
            std::string label = qol_capture == i ? text("press a key...", "pulsa una tecla...") : (name && *name ? name : "?");
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(i ? text("R bar backwards (RT)", "Barra R hacia atr\xC3\xA1s (RT)") : text("L bar backwards (LT)", "Barra L hacia atr\xC3\xA1s (LT)"));
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Button((label + "##qol").c_str(), ImVec2(180, 0)))
                qol_capture = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (qol_capture >= 0) {             /* the box waits: the first key held is the new one, Esc cancels */
        int count = 0;
        const bool *state = SDL_GetKeyboardState(&count);
        for (int code = 1; code < count && qol_capture >= 0; code++)
            if (state[code]) {
                if (code != SDL_SCANCODE_ESCAPE) {
                    keys[qol_capture] = code;
                    settings["keys_qol"] = std::to_string(keys[0]) + "," + std::to_string(keys[1]);
                    save_settings();
                }
                qol_capture = -1;
            }
    }
    if (ImGui::Button(text("Restore default keys (D, F)", "Restaurar teclas (D, F)"))) {
        settings.erase("keys_qol");
        save_settings();
        qol_capture = -1;
    }
}
