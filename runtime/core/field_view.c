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

enum { STATE = 0x03003170, LAYER_SIZE = 0x18 };

/* Is the hardware layer really showing this map? The engine copies the visible part of the map into the layer's
 * 32 x 32 tile ring, so tile (tx, ty) of the map sits at ring position (tx mod 32, ty mod 32). A menu drawn
 * on the same layer (status, items) leaves the field's records in place but fills the ring with its own tiles;
 * reading the map then would draw the field's tiles where the menu is. Every tile on screen is compared; the
 * engine itself changes a few (doors, switches), so a small part may differ. (The console's own columns are
 * drawn from the ring in any case, see ppu.c: this only decides whether the sides continue the field.) */
static int shows_its_map(int bg, const uint16_t *map, int map_w, int map_h, int scroll_x, int scroll_y)
{
    const uint16_t *ring = (const uint16_t *)(uintptr_t)(SS_VRAM + ((IO16(0x08 + bg * 2) >> 8) & 31) * 0x800u);
    int sx, sy, checked = 0, wrong = 0;
    if (IO16(0x08 + bg * 2) & 0xC000)
        return 1;                       /* larger hardware maps are not rings of 32: not judged */
    for (sy = 0; sy < 20; sy++) {
        for (sx = 0; sx < 30; sx++) {
            int tx = (scroll_x >> 3) + sx, ty = (scroll_y >> 3) + sy;
            if (tx < 0 || ty < 0 || tx >= map_w || ty >= map_h)
                continue;
            checked++;
            wrong += ring[(ty & 31) * 32 + (tx & 31)] != map[ty * map_w + tx];
        }
    }
    return checked < 6 || wrong * 16 <= checked;
}

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
        if (!shows_its_map(bg, (const uint16_t *)(uintptr_t)data, map_w, map_h, x, y))
            return 0;                   /* a menu took the layer over: the whole scene keeps the console's picture */
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
    /* The console's 240 columns stay in the middle of the view, always: world, sprites, menus and effects then
     * share one offset whatever the camera does. (An earlier version slid the view at the edges of a map to
     * show more of it; menus drawn with sprites then parted from their backgrounds.) Where the view reaches
     * past the widest map there is nothing to show, and the renderer paints it black: hud_left and hud_right
     * are the columns the map covers. The status bar is not divided. */
    extra = width - SS_SCREEN_W;
    view->shift = extra / 2;
    view->hud_rows = 0;
    view->hud_left = 0;
    view->hud_right = width;
    for (i = 0; i < 4; i++) {
        if (view->bg[i].map && view->bg[i].width * 8 == widest) {
            origin = view->bg[i].scroll_x - view->shift;        /* world x of the view's left column */
            if (origin < 0)
                view->hud_left = -origin;
            if (origin + width > widest)
                view->hud_right = widest - origin;
            if (view->hud_left > view->shift) view->hud_left = view->shift;                      /* never into the console's own columns */
            if (view->hud_right < view->shift + SS_SCREEN_W) view->hud_right = view->shift + SS_SCREEN_W;
            break;
        }
    }
    return 1;
}
