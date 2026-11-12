/* Software renderer for the GBA picture processor.
 *
 * Draws one frame from the video registers, palette, VRAM and OAM into a 32-bit buffer. Implemented: text
 * backgrounds (modes 0-2), affine background 2 (modes 1-2) and 3 (mode 2), regular and affine sprites,
 * priorities, the two windows and the sprite window, alpha blending and brightness fades.
 * Whole-frame rendering for now: effects that change registers per scanline are a later step.
 *
 * Wide view. The picture can be wider than the console's 240 pixels (struct ss_view). A background can be given
 * its full source map, so the extra columns show real scenery instead of the 256-pixel hardware ring; the
 * other backgrounds and the sprites keep their native composition, placed by these rules:
 *   - world layers and sprites move with the view (`shift` is where native x = 0 lands);
 *   - the top `hud_rows` lines are the HUD: their left half stays at the left edge, their right half goes to
 *     the right edge;
 *   - everything else that has no source map is centred.
 */
#include "shiningsource/runtime.h"

#include <string.h>

#define IO16(o)  (*(const uint16_t *)(uintptr_t)(SS_IO + (o)))
#define IO32(o)  (*(const uint32_t *)(uintptr_t)(SS_IO + (o)))
#define PAL      ((const uint16_t *)(uintptr_t)SS_PALETTE)
#define VRAM     ((const uint8_t *)(uintptr_t)SS_VRAM)
#define OAM      ((const uint16_t *)(uintptr_t)SS_OAM)

enum { W = SS_SCREEN_W, H = SS_SCREEN_H, MAXW = SS_MAX_VIEW_W, LAYER_OBJ = 4, LAYER_BACKDROP = 5 };

/* per scanline, per layer: palette index + 1 (0 = transparent); sprites also carry priority and flags */
static uint16_t line_bg[4][MAXW];
static uint16_t line_obj[MAXW];
static uint8_t line_obj_prio[MAXW], line_obj_flags[MAXW];   /* flags: 1 semi-transparent, 2 sprite-window pixel */

static struct ss_view view;

/* Native x (0..239) shown at view column X for content without a source map, or -1 if nothing is there. */
static int native_x(int X, int y)
{
    int extra = view.width - W, x;
    if (y < view.hud_rows) {            /* halves of the HUD at the edges of what is visible */
        x = X - view.hud_left;
        if (x >= 0 && x < W / 2) return x;
        x = X - (view.hud_right - W);
        return (x >= W / 2 && x < W) ? x : -1;
    }
    x = X - extra / 2;
    return (x >= 0 && x < W) ? x : -1;
}

static uint16_t tile_pixel(const uint8_t *chars, uint16_t entry, int tx, int ty, int colour256)
{
    int index;
    if (entry & 0x400) tx = 7 - tx;
    if (entry & 0x800) ty = 7 - ty;
    if (colour256) {
        index = chars[(entry & 0x3FF) * 64 + ty * 8 + tx];
        return (uint16_t)(index ? index + 1 : 0);
    }
    index = chars[(entry & 0x3FF) * 32 + ty * 4 + (tx >> 1)];
    index = (tx & 1) ? index >> 4 : index & 15;
    return (uint16_t)(index ? (entry >> 12) * 16 + index + 1 : 0);
}

static void text_background(int n, int y)
{
    uint16_t cnt = IO16(0x08 + n * 2);
    const uint8_t *chars = VRAM + ((cnt >> 2) & 3) * 0x4000;
    const uint8_t *map = VRAM + ((cnt >> 8) & 31) * 0x800;
    int colour256 = cnt & 0x80, size = cnt >> 14;
    int width = (size & 1) ? 512 : 256, height = (size & 2) ? 512 : 256;
    int scroll_x = IO16(0x10 + n * 4) & 511, scroll_y = IO16(0x12 + n * 4) & 511;
    const struct ss_bg_source *source = &view.bg[n];
    int X;

    if (source->map) {              /* full source map: world position, no wrapping */
        int wy = y + source->scroll_y;
        for (X = 0; X < view.width; X++) {
            int wx = X - view.shift + source->scroll_x;
            if (wx < 0 || wy < 0 || (wx >> 3) >= source->width || (wy >> 3) >= source->height)
                line_bg[n][X] = 0;
            else
                line_bg[n][X] = tile_pixel(chars, source->map[(wy >> 3) * source->width + (wx >> 3)], wx & 7, wy & 7, colour256);
        }
        return;
    }
    for (X = 0; X < view.width; X++) {
        int x = native_x(X, y), px, py, block;
        if (x < 0) {
            line_bg[n][X] = 0;
            continue;
        }
        px = (x + scroll_x) & (width - 1);
        py = (y + scroll_y) & (height - 1);
        block = (px >> 8) + ((py >> 8) * (width == 512 ? 2 : 1));
        line_bg[n][X] = tile_pixel(chars, *(const uint16_t *)(map + block * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2),
                                   px & 7, py & 7, colour256);
    }
}

static void affine_background(int n, int y)
{
    uint16_t cnt = IO16(0x08 + n * 2);
    const uint8_t *chars = VRAM + ((cnt >> 2) & 3) * 0x4000;
    const uint8_t *map = VRAM + ((cnt >> 8) & 31) * 0x800;
    int size = 128 << (cnt >> 14), wrap = cnt & 0x2000, X;
    int base = 0x20 + (n - 2) * 0x10;
    int32_t pa = (int16_t)IO16(base), pb = (int16_t)IO16(base + 2), pc = (int16_t)IO16(base + 4), pd = (int16_t)IO16(base + 6);
    int32_t ref_x = (int32_t)(IO32(base + 8) << 4) >> 4, ref_y = (int32_t)(IO32(base + 12) << 4) >> 4;

    for (X = 0; X < view.width; X++) {
        int x = native_x(X, y), px, py, index;
        if (x < 0) {
            line_bg[n][X] = 0;
            continue;
        }
        px = (ref_x + pb * y + pa * x) >> 8;
        py = (ref_y + pd * y + pc * x) >> 8;
        if (wrap) {
            px &= size - 1;
            py &= size - 1;
        } else if (px < 0 || py < 0 || px >= size || py >= size) {
            line_bg[n][X] = 0;
            continue;
        }
        index = chars[map[(py >> 3) * (size >> 3) + (px >> 3)] * 64 + (py & 7) * 8 + (px & 7)];
        line_bg[n][X] = (uint16_t)(index ? index + 1 : 0);
    }
}

static void sprites(int y)
{
    static const uint8_t sizes[3][4][2] = {
        { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
        { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
        { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
    };
    uint16_t dispcnt = IO16(0);
    int one_dimensional = dispcnt & 0x40, extra = view.width - W, i;

    memset(line_obj, 0, sizeof line_obj);
    memset(line_obj_prio, 4, sizeof line_obj_prio);
    memset(line_obj_flags, 0, sizeof line_obj_flags);
    for (i = 127; i >= 0; i--) {
        uint16_t a0 = OAM[i * 4], a1 = OAM[i * 4 + 1], a2 = OAM[i * 4 + 2];
        int affine = a0 & 0x100, mode = (a0 >> 10) & 3, shape = a0 >> 14;
        int width, height, box_w, box_h, x0, y0, row, colour256, tile, prio, bank, x, place;
        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;

        if ((!affine && (a0 & 0x200)) || shape == 3)
            continue;
        width = sizes[shape][a1 >> 14][0];
        height = sizes[shape][a1 >> 14][1];
        box_w = width;
        box_h = height;
        if (affine && (a0 & 0x200)) {
            box_w *= 2;
            box_h *= 2;
        }
        y0 = a0 & 255;
        if (y0 >= 160) y0 -= 256;
        row = y - y0;
        if (row < 0 || row >= box_h)
            continue;
        x0 = a1 & 511;
        /* On the console anything from 240 up is off screen or a wrapped negative position. With a wider view a
         * sprite just right of 240 is visible, so only the top of the range is read as negative. */
        if (x0 >= (extra ? 512 - 128 : 240)) x0 -= 512;
        /* where this sprite goes in the view: HUD sprites to their edge, sprites of a scene with a source map
         * with the world, anything else centred */
        if (y0 + box_h <= view.hud_rows && view.hud_rows)
            place = (x0 + box_w / 2 < W / 2) ? view.hud_left : view.hud_right - W;
        else
            place = view.world ? view.shift : extra / 2;
        colour256 = a0 & 0x2000;
        tile = a2 & 0x3FF;
        prio = (a2 >> 10) & 3;
        bank = a2 >> 12;
        if (affine) {
            const uint16_t *matrix = OAM + ((a1 >> 9) & 31) * 16;
            pa = (int16_t)matrix[3]; pb = (int16_t)matrix[7]; pc = (int16_t)matrix[11]; pd = (int16_t)matrix[15];
        }
        for (x = 0; x < box_w; x++) {
            int sx = x0 + x + place, tx, ty, number, index;
            if (sx < 0 || sx >= view.width)
                continue;
            if (affine) {
                int dx = x - box_w / 2, dy = row - box_h / 2;
                tx = ((pa * dx + pb * dy) >> 8) + width / 2;
                ty = ((pc * dx + pd * dy) >> 8) + height / 2;
                if (tx < 0 || ty < 0 || tx >= width || ty >= height)
                    continue;
            } else {
                tx = (a1 & 0x1000) ? width - 1 - x : x;
                ty = (a1 & 0x2000) ? height - 1 - row : row;
            }
            if (colour256) {
                number = tile + (ty >> 3) * (one_dimensional ? (width >> 3) * 2 : 32) + (tx >> 3) * 2;
                index = VRAM[0x10000 + (number & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7)];
            } else {
                uint8_t byte;
                number = tile + (ty >> 3) * (one_dimensional ? (width >> 3) : 32) + (tx >> 3);
                byte = VRAM[0x10000 + (number & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)];
                index = (tx & 1) ? byte >> 4 : byte & 15;
                if (index) index += bank * 16;
            }
            if (!index)
                continue;
            if (mode == 2) {
                line_obj_flags[sx] |= 2;
                continue;
            }
            if (prio <= line_obj_prio[sx] || !line_obj[sx]) {
                line_obj[sx] = (uint16_t)(0x100 + index + 1);
                line_obj_prio[sx] = (uint8_t)prio;
                line_obj_flags[sx] = (uint8_t)((line_obj_flags[sx] & 2) | (mode == 1));
            }
        }
    }
}

static uint32_t to_rgb(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static uint16_t blend(uint16_t top, uint16_t bottom, int eva, int evb)
{
    int r = ((top & 31) * eva + (bottom & 31) * evb) >> 4;
    int g = (((top >> 5) & 31) * eva + ((bottom >> 5) & 31) * evb) >> 4;
    int b = (((top >> 10) & 31) * eva + ((bottom >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (uint16_t)(r | g << 5 | b << 10);
}

static uint16_t fade(uint16_t c, int amount, int to_white)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (to_white) {
        r += ((31 - r) * amount) >> 4; g += ((31 - g) * amount) >> 4; b += ((31 - b) * amount) >> 4;
    } else {
        r -= (r * amount) >> 4; g -= (g * amount) >> 4; b -= (b * amount) >> 4;
    }
    return (uint16_t)(r | g << 5 | b << 10);
}

static int inside(int low, int high, int value)
{
    return low <= high ? (value >= low && value < high) : (value >= low || value < high);
}

void ss_ppu_render(uint32_t *pixels, int pitch_pixels, const struct ss_view *requested)
{
    uint16_t dispcnt = IO16(0), bldcnt = IO16(0x50);
    int mode = dispcnt & 7, eva = IO16(0x52) & 31, evb = (IO16(0x52) >> 8) & 31, evy = IO16(0x54) & 31;
    int windows = dispcnt & 0xE000, y, X, n;
    int prio[4], enabled[4];

    if (requested) {
        view = *requested;
    } else {
        memset(&view, 0, sizeof view);
        view.width = W;
    }
    if (!view.hud_right) view.hud_right = view.width;
    if (view.width < W) view.width = W;
    if (view.width > MAXW) view.width = MAXW;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    for (n = 0; n < 4; n++) {
        prio[n] = IO16(0x08 + n * 2) & 3;
        enabled[n] = (dispcnt & (0x100 << n)) && ((mode == 0) || (mode == 1 && n < 3) || (mode == 2 && n >= 2));
    }
    for (y = 0; y < H; y++) {
        uint32_t *out = pixels + y * pitch_pixels;
        if (dispcnt & 0x80) {
            for (X = 0; X < view.width; X++) out[X] = 0xFFFFFFFFu;
            continue;
        }
        for (n = 0; n < 4; n++) {
            if (!enabled[n]) continue;
            if ((mode == 1 && n == 2) || mode == 2) affine_background(n, y);
            else text_background(n, y);
        }
        if (dispcnt & 0x1000) sprites(y);
        for (X = 0; X < view.width; X++) {
            int mask = 0x3F, top = LAYER_BACKDROP, second = LAYER_BACKDROP, p;
            uint16_t top_colour = PAL[0], second_colour = PAL[0];
            int obj_visible, semi = 0;

            if (!view.world && view.width > W && native_x(X, y) < 0 && y >= view.hud_rows) {
                out[X] = 0xFF000000u;       /* side bars of a scene that keeps the console's composition */
                continue;
            }
            if (windows) {
                /* window coordinates are native; the columns beyond the console's screen take the edge value, so a
                 * full-screen mask (the scene-change blackout) covers them too */
                int x = X - (view.world ? view.shift : (view.width - W) / 2);
                uint16_t winin = IO16(0x48), winout = IO16(0x4A);
                if (x < 0) x = 0;
                if (x > W - 1) x = W - 1;
                if ((dispcnt & 0x2000) && inside(IO16(0x44) >> 8, IO16(0x44) & 255, y) && inside(IO16(0x40) >> 8, IO16(0x40) & 255, x))
                    mask = winin & 0x3F;
                else if ((dispcnt & 0x4000) && inside(IO16(0x46) >> 8, IO16(0x46) & 255, y) && inside(IO16(0x42) >> 8, IO16(0x42) & 255, x))
                    mask = (winin >> 8) & 0x3F;
                else if ((dispcnt & 0x8000) && (dispcnt & 0x1000) && (line_obj_flags[X] & 2))
                    mask = (winout >> 8) & 0x3F;
                else
                    mask = winout & 0x3F;
            }
            obj_visible = (dispcnt & 0x1000) && (mask & 0x10) && line_obj[X];
            /* walk priorities from the back so that `top` ends as the front-most pixel and `second` as the one under it */
            for (p = 3; p >= 0; p--) {
                for (n = 3; n >= 0; n--) {
                    if (enabled[n] && prio[n] == p && (mask & (1 << n)) && line_bg[n][X]) {
                        second = top; second_colour = top_colour;
                        top = n; top_colour = PAL[line_bg[n][X] - 1];
                    }
                }
                if (obj_visible && line_obj_prio[X] == p) {
                    second = top; second_colour = top_colour;
                    top = LAYER_OBJ; top_colour = PAL[line_obj[X] - 1];
                    semi = line_obj_flags[X] & 1;
                }
            }
            if (semi && (bldcnt & (0x100 << second))) {
                top_colour = blend(top_colour, second_colour, eva, evb);
            } else if ((mask & 0x20) && (bldcnt & (1 << top))) {
                switch ((bldcnt >> 6) & 3) {
                case 1:
                    if (bldcnt & (0x100 << second)) top_colour = blend(top_colour, second_colour, eva, evb);
                    break;
                case 2: top_colour = fade(top_colour, evy, 1); break;
                case 3: top_colour = fade(top_colour, evy, 0); break;
                }
            }
            out[X] = to_rgb(top_colour);
        }
    }
}
