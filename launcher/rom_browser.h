#pragma once

#include <filesystem>

namespace rom_browser {
namespace fs = std::filesystem;

class Picker {
public:
    void open(const std::string &selected, const std::string &install)
    {
        location = selected.empty() ? fs::path(install) : fs::path(selected).parent_path();
        refresh();
        ImGui::OpenPopup("rom-browser");
    }

    std::string draw(const char *title, const char *up, const char *drives, const char *cancel, const char *empty)
    {
        std::string chosen;
        ImGui::SetNextWindowSize(ImVec2(620, 425), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("rom-browser", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) return chosen;
        ImGui::TextUnformatted(title);
        ImGui::TextWrapped("%s", location.empty() ? drives : location.u8string().c_str());
        if (!location.empty() && soul_ui::button(up)) {
            const auto parent = location.parent_path();
            location = parent == location ? fs::path() : parent;
            refresh();
        }
        ImGui::BeginChild("rom-entries", ImVec2(0, -44), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        if (new_folder) {
            ImGui::SetScrollY(0);
            new_folder = false;
        }
        if (entries.empty()) ImGui::TextDisabled("%s", empty);
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto &entry = entries[i];
            const auto label = (entry.directory ? ">  " : "    ") + (location.empty() ? entry.path.u8string() : entry.path.filename().u8string());
            ImGui::PushID((int)i);
            const bool pressed = ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, 26));
            if (i == 0 && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
            if (!pressed) continue;
            if (entry.directory) {
                location = entry.path;
                refresh();
            } else {
                // The standalone launcher uses the Windows ANSI path in its INI.
                const auto native = entry.path.native();
                char path[MAX_PATH];
                BOOL substituted = FALSE;
                if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, native.c_str(), -1,
                                         path, sizeof path, nullptr, &substituted) && !substituted) {
                    chosen = path;
                    ImGui::CloseCurrentPopup();
                } else {
                    path_error = true;
                }
            }
            break;
        }
        ImGui::EndChild();
        if (path_error) ImGui::TextDisabled("%s", "Use a shorter path in the Windows language / Usa una ruta mas corta en el idioma de Windows");
        if (soul_ui::button(cancel) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return chosen;
    }

private:
    struct Entry { fs::path path; bool directory; };
    fs::path location;
    std::vector<Entry> entries;
    bool new_folder = false, path_error = false;

    void refresh()
    {
        entries.clear();
        new_folder = true;
        path_error = false;
        if (location.empty()) {
            DWORD drives = GetLogicalDrives();
            for (int i = 0; i < 26; ++i) {
                if (!(drives & (1u << i))) continue;
                wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
                const UINT type = GetDriveTypeW(root);
                if (type != DRIVE_CDROM && type != DRIVE_UNKNOWN && type != DRIVE_NO_ROOT_DIR) entries.push_back({ root, true });
            }
            return;
        }
        std::error_code error;
        for (fs::directory_iterator it(location, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code type_error;
            const bool dir = it->is_directory(type_error);
            if (dir || (it->is_regular_file(type_error) && !_wcsicmp(it->path().extension().c_str(), L".gba")))
                entries.push_back({ it->path(), dir });
        }
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            return a.directory != b.directory ? a.directory : _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
        });
    }
};

} // namespace rom_browser
