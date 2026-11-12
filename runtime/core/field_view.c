/* Wide view for the Shining Soul engine: tell the renderer where the field's full maps are.
 *
 * This is the one part of the core that knows the engine's data. The field keeps its state at 0x03003170
 * (struct LoadState in the decompilation): four layer records, each with a pointer to a complete tile map
 * (16-bit screen entries, `width` x `height` tiles), and the camera scroll. The engine copies a 256-pixel
 * window of each map into the hardware tile ring; a wider view reads the maps directly instead.
 *
 * A layer is used only if the position computed here agrees with the scroll register the game wrote, so a
 * scene that drives a layer some other way falls back to the console's composition by itself.
 */
#include "shiningsource/runtime.h"

#include <string.h>

#define RD16(a) (*(const uint16_t *)(uintptr_t)(a))
#define RD32(a) (*(const uint32_t *)(uintptr_t)(a))
#define IO16(o) RD16(SS_IO + (o))

enum { STATE = 0x03003170, LAYER_SIZE = 0x18, HUD_ROWS = 16 };

/* Fill `view` for a picture `width` pixels wide. Returns 1 when the scene has source maps (a field scene). */
int ss_field_view(struct ss_view *view, int width)
{
    int scroll_x = (int16_t)RD16(STATE + 0x7C), scroll_y = (int16_t)RD16(STATE + 0x7E);
    int offset_x = (int16_t)RD16(STATE + 0x84), offset_y = (int16_t)RD16(STATE + 0x86);
    int i, found = 0, widest = 0, extra, origin;

    memset(view, 0, sizeof *view);
    view->width = width;
    if ((IO16(0) & 7) != 0)
        return 0;
    for (i = 0; i < 4; i++) {
        uint32_t layer = STATE + (uint32_t)i * LAYER_SIZE, data = RD32(layer + 0x0C);
        int bg = RD16(layer + 0x14), relative = RD16(layer + 0x16);
        int map_w = RD16(layer + 0x10), map_h = RD16(layer + 0x12);
        int x, y;
        if (data < SS_EWRAM || data >= SS_ROM + 0x1000000 || bg > 3 || map_w < 30 || map_h < 20 || map_w > 1024 || map_h > 1024)
            continue;
        x = (int16_t)RD16(layer + 0x08) + (relative ? scroll_x : 0);
        y = (int16_t)RD16(layer + 0x0A) + (relative ? scroll_y : 0);
        x += offset_x + (int16_t)RD16(layer + 0x04);
        y += offset_y + (int16_t)RD16(layer + 0x06);
        if (((x ^ IO16(0x10 + bg * 4)) & 0xFF) || ((y ^ IO16(0x12 + bg * 4)) & 0xFF))
            continue;                   /* not where the game put it this frame */
        view->bg[bg].map = (const uint16_t *)(uintptr_t)data;
        view->bg[bg].width = map_w;
        view->bg[bg].height = map_h;
        view->bg[bg].scroll_x = x;
        view->bg[bg].scroll_y = y;
        if (map_w * 8 > widest)
            widest = map_w * 8;
        found = 1;
    }
    if (!found)
        return 0;
    view->world = 1;
    view->hud_rows = HUD_ROWS;
    view->hud_left = 0;
    view->hud_right = width;
    /* Centre the console's 240 columns in the view, but never show past the widest map: at a map edge the
     * view slides, exactly as the game's own camera does at its edges. */
    extra = width - SS_SCREEN_W;
    view->shift = extra / 2;
    for (i = 0; i < 4; i++) {
        if (view->bg[i].map && view->bg[i].width * 8 == widest) {
            origin = view->bg[i].scroll_x - view->shift;        /* world x of the view's left column */
            if (widest <= width) {
                origin = (widest - width) / 2;                  /* map narrower than the view: centre the map */
                view->hud_left = -origin;
                view->hud_right = -origin + widest;
                if (view->hud_right - view->hud_left < SS_SCREEN_W) {
                    view->hud_left = extra / 2;
                    view->hud_right = view->hud_left + SS_SCREEN_W;
                }
            }
            else if (origin < 0)
                origin = 0;
            else if (origin + width > widest)
                origin = widest - width;
            view->shift = view->bg[i].scroll_x - origin;
            break;
        }
    }
    return 1;
}
