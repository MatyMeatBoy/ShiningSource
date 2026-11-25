#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace soul_ui {

constexpr float title_height = 48.0f;
constexpr float chrome_width = 46.0f;
constexpr ImU32 brass = IM_COL32(202, 165, 100, 255);
constexpr ImU32 ink = IM_COL32(12, 24, 22, 255);
constexpr ImU32 cream = IM_COL32(245, 235, 211, 255);
static GLuint grain, cover;
static float cover_aspect = 1;
static ImFont *heading, *body;

static GLuint texture(const void *pixels, int width, int height)
{
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return id;
}

static void init()
{
    // A fixed grain over a beveled metal gradient. It is shared by every button.
    constexpr int width = 256, height = 48;
    std::vector<uint8_t> pixels(width * height * 4);
    uint32_t state = 0x352bf741;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            state = state * 1664525u + 1013904223u;
            int value = 255 - (y * 78 / height) + (int)(state >> 28) - 8;
            int at = (y * width + x) * 4;
            pixels[at] = pixels[at + 1] = pixels[at + 2] = (uint8_t)std::clamp(value, 0, 255);
            pixels[at + 3] = 255;
        }
    }
    grain = texture(pixels.data(), width, height);
    const char *base = SDL_GetBasePath();
    SDL_Surface *image = base ? SDL_LoadPNG((std::string(base) + "assets/launcher/cover.png").c_str()) : nullptr;
    if (image) {
        SDL_Surface *rgba = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(image);
        if (rgba) {
            cover_aspect = (float)rgba->w / rgba->h;
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
            cover = texture(rgba->pixels, rgba->w, rgba->h);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            SDL_DestroySurface(rgba);
        }
    }
}

static ImTextureID id(GLuint handle) { return (ImTextureID)(intptr_t)handle; }

static void style()
{
    ImGuiIO &io = ImGui::GetIO();
    char windows[MAX_PATH];
    if (GetWindowsDirectoryA(windows, MAX_PATH)) {
        const std::string fonts = std::string(windows) + "\\Fonts\\";
        if (SDL_PathInfo info; SDL_GetPathInfo((fonts + "segoeui.ttf").c_str(), &info))
            body = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 17.0f);
        if (!body)
            body = io.Fonts->AddFontDefault();
        if (SDL_PathInfo info; SDL_GetPathInfo((fonts + "georgia.ttf").c_str(), &info))
            heading = io.Fonts->AddFontFromFileTTF((fonts + "georgia.ttf").c_str(), 31.0f);
    }
    io.FontDefault = body;
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = s.ChildRounding = s.PopupRounding = 14;
    s.FrameRounding = s.GrabRounding = 9;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = s.FrameBorderSize = 1;
    s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(10, 9);
    s.WindowPadding = ImVec2(16, 14);
    s.ScrollbarSize = 11;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(.080f, .132f, .113f, 1);
    s.Colors[ImGuiCol_Border] = ImVec4(.34f, .38f, .27f, 1);
    s.Colors[ImGuiCol_Text] = ImVec4(.96f, .93f, .86f, 1);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(.56f, .64f, .58f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(.18f, .29f, .23f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(.24f, .38f, .29f, 1);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(.37f, .45f, .30f, 1);
    s.Colors[ImGuiCol_Header] = ImVec4(.27f, .38f, .28f, 1);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(.31f, .43f, .32f, 1);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(.36f, .48f, .33f, 1);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.12f, .21f, .18f, 1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.18f, .30f, .23f, 1);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(.24f, .37f, .28f, 1);
    s.Colors[ImGuiCol_CheckMark] = s.Colors[ImGuiCol_NavCursor] = ImVec4(.99f, .83f, .48f, 1);
    s.Colors[ImGuiCol_SliderGrab] = ImVec4(.76f, .62f, .35f, 1);
    s.Colors[ImGuiCol_SliderGrabActive] = s.Colors[ImGuiCol_CheckMark];
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.082f, .148f, .124f, 1);
    s.Colors[ImGuiCol_TableHeaderBg] = ImVec4(.13f, .22f, .18f, 1);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, .16f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(.38f, .45f, .34f, 1);
    s.Colors[ImGuiCol_Separator] = ImVec4(.33f, .37f, .27f, 1);
}

static void tab_icon(ImDrawList *draw, ImVec2 center, int icon, ImU32 color)
{
    const auto point = [&](float x, float y) { return ImVec2(center.x + x, center.y + y); };
    const auto line = [&](float x, float y, float xx, float yy) { draw->AddLine(point(x, y), point(xx, yy), color, 1.25f); };
    switch (icon) {
    case 0:
        draw->AddTriangle(point(-5, 0), point(0, -6), point(5, 0), color, 1.25f);
        draw->AddRect(point(-4, 0), point(4, 5), color);
        break;
    case 1:
        draw->AddRect(point(-6, -5), point(6, 3), color, 1.5f);
        line(0, 3, 0, 6); line(-3, 6, 3, 6);
        break;
    case 2:
        line(-1, 3, -1, -5); line(-1, -5, 5, -6); line(5, -6, 5, 2);
        draw->AddCircleFilled(point(-3, 3), 2, color);
        draw->AddCircleFilled(point(3, 2), 2, color);
        break;
    case 3:
        draw->AddRect(point(-7, -4), point(7, 4), color, 3);
        line(-5, 0, -1, 0); line(-3, -2, -3, 2);
        draw->AddCircleFilled(point(3, 0), 1, color); draw->AddCircleFilled(point(5, -1), 1, color);
        break;
    case 4:
        draw->AddCircle(center, 6, color, 16, 1.25f);
        draw->AddEllipse(center, ImVec2(3, 6), color, 0, 12, 1.25f);
        line(-6, 0, 6, 0);
        break;
    case 5:
        draw->AddCircle(point(0, -2), 4, color, 12, 1.25f);
        line(-2, 2, -4, 6); line(2, 2, 4, 6);
        break;
    case 6:
        line(-5, -4, 5, -4); line(-5, 0, 5, 0); line(-5, 4, 5, 4);
        draw->AddCircleFilled(point(-2, -4), 1.5f, color); draw->AddCircleFilled(point(2, 0), 1.5f, color);
        draw->AddCircleFilled(point(0, 4), 1.5f, color);
        break;
    case 7:
        draw->AddRect(point(-5, -6), point(5, 6), color, 1.5f);
        line(-2, -2, 2, -2); line(-2, 1, 2, 1);
        break;
    default:
        draw->AddCircle(center, 6, color, 16, 1.25f);
        draw->AddCircleFilled(point(0, -3), 1, color); line(0, -1, 0, 3);
        break;
    }
}

static bool button(const char *label, ImVec2 size = ImVec2(0, 0), bool primary = false, bool selected = false, bool left = false, int icon = -1)
{
    ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    if (size.x < 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.x == 0) size.x = label_size.x + 28;
    if (size.y == 0) size.y = label_size.y + 14;
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsItemHovered(), focus = ImGui::IsItemFocused();
    const ImU32 colour = primary ? (hover ? IM_COL32(242, 205, 131, 255) : IM_COL32(224, 179, 100, 255)) :
                         selected ? IM_COL32(66, 100, 75, 255) : hover ? IM_COL32(56, 86, 68, 255) : IM_COL32(39, 61, 50, 255);
    const ImU32 fill = ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(colour));
    draw->AddImageRounded(id(grain), lo, hi, ImVec2(0, 0), ImVec2(1, 1), fill, 10);
    draw->AddRect(lo, hi, ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(focus || primary || selected ? brass : IM_COL32(79, 102, 81, 255))), 10, 0, focus ? 2.0f : 1.0f);
    const char *end = label + strcspn(label, "#");
    if (icon >= 0) tab_icon(draw, ImVec2(lo.x + 19, lo.y + size.y * .5f), icon, brass);
    draw->AddText(ImVec2(lo.x + (left ? (icon >= 0 ? 34 : 14) : (size.x - label_size.x) * .5f), lo.y + (size.y - label_size.y) * .5f),
                  ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(primary ? ink : cream)), label, end);
    return pressed;
}

static void backdrop()
{
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(29, 50, 42, 255), IM_COL32(21, 38, 32, 255),
                                 IM_COL32(12, 23, 20, 255), IM_COL32(12, 23, 20, 255));
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, title_height), ink);
    draw->AddLine(ImVec2(0, title_height), ImVec2(size.x, title_height), IM_COL32(93, 105, 72, 255));
}

static void banner(const char *title, const char *region, bool show_cover)
{
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const ImVec2 hi(lo.x + ImGui::GetContentRegionAvail().x, lo.y + 178);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddImageRounded(id(grain), lo, hi, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(35, 61, 46, 255), 12);
    draw->AddRect(lo, hi, IM_COL32(109, 120, 79, 255), 12);
    if (cover && show_cover) {
        constexpr float art_height = 150;
        const ImVec2 right(hi.x - 14, hi.y - 14);
        const ImVec2 left(right.x - art_height * cover_aspect, right.y - art_height);
        draw->AddRectFilled(ImVec2(left.x + 3, left.y + 4), ImVec2(right.x + 3, right.y + 4), IM_COL32(4, 16, 12, 160), 8);
        draw->AddImageRounded(id(cover), left, right, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8);
        draw->AddRect(left, right, brass, 8);
    }
    if (heading) ImGui::PushFont(heading);
    draw->AddText(ImVec2(lo.x + 23, lo.y + 56), cream, title);
    if (heading) ImGui::PopFont();
    draw->AddText(ImVec2(lo.x + 24, lo.y + 101), IM_COL32(220, 206, 164, 255), region);
    ImGui::Dummy(ImVec2(hi.x - lo.x, hi.y - lo.y + 6));
}

static void section(const char *title)
{
    if (heading) ImGui::PushFont(heading);
    ImGui::TextUnformatted(title);
    if (heading) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *window, const SDL_Point *p, void *)
{
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_FULLSCREEN) return SDL_HITTEST_NORMAL;
    if (!(flags & SDL_WINDOW_MAXIMIZED)) {
        constexpr int edge = 6;
        bool left = p->x < edge, right = p->x >= w - edge;
        if (p->y < edge) return left ? SDL_HITTEST_RESIZE_TOPLEFT : right ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
        if (p->y >= h - edge) return left ? SDL_HITTEST_RESIZE_BOTTOMLEFT : right ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }
    return p->y < title_height && p->x < w - 3 * chrome_width ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

static void chrome(SDL_Window *window, bool &running)
{
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float width = ImGui::GetIO().DisplaySize.x;
    draw->AddRectFilled(ImVec2(18, 11), ImVec2(46, 37), IM_COL32(72, 91, 60, 255), 7);
    draw->AddText(ImVec2(28, 13), brass, "II");
    draw->AddText(ImVec2(56, 14), cream, "ShiningSource");
    draw->AddText(ImVec2(180, 14), IM_COL32(165, 175, 153, 255), "0.9.4");
    const char *labels[] = { "##minimize", "##maximize", "##close" };
    for (int i = 0; i < 3; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(width - (3 - i) * chrome_width, 6));
        if (ImGui::InvisibleButton(labels[i], ImVec2(chrome_width - 6, title_height - 8))) {
            if (i == 0) SDL_MinimizeWindow(window);
            if (i == 1) {
                if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
                else SDL_MaximizeWindow(window);
            }
            if (i == 2) running = false;
        }
        const ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered())
            draw->AddRectFilled(p, q, i == 2 ? IM_COL32(117, 56, 49, 255) : IM_COL32(49, 72, 56, 255), 7);
        const ImVec2 c((p.x + q.x) * .5f, (p.y + q.y) * .5f);
        if (i == 0) draw->AddLine(ImVec2(c.x - 5, c.y + 2), ImVec2(c.x + 5, c.y + 2), cream);
        if (i == 1) draw->AddRect(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), cream);
        if (i == 2) {
            draw->AddLine(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), cream);
            draw->AddLine(ImVec2(c.x + 4, c.y - 4), ImVec2(c.x - 4, c.y + 4), cream);
        }
    }
}

} // namespace soul_ui
