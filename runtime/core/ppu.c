/* Software renderer for the GBA picture processor.
 *
 * Draws one frame from the video registers, palette, VRAM and OAM into a 32-bit buffer. Implemented: text
 * backgrounds (modes 0-2), affine background 2 (modes 1-2) and 3 (mode 2), regular and affine sprites,
 * priorities, the two windows and the sprite window, alpha blending and brightness fades.
 * Registers that the game changes at a scanline (its line-match interrupt, see frame.c) take effect from that
 * line on: each line is drawn with the registers as they were on it.
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

/* the field's status display in a wide view: its lines, and the column that parts its left and right groups */
enum { HUD_BAND = 56, HUD_SPLIT = 176, EDGE_FADE = 12 };
enum { W = SS_SCREEN_W, H = SS_SCREEN_H, MAXW = SS_MAX_VIEW_W, LAYER_OBJ = 4, LAYER_BACKDROP = 5 };

/* per scanline, per layer: palette index + 1 (0 = transparent); sprites also carry priority and flags */
static uint16_t line_bg[4][MAXW];
static uint16_t line_obj[MAXW];
static uint8_t line_obj_prio[MAXW], line_obj_flags[MAXW];   /* flags: 1 semi-transparent, 2 sprite-window pixel */
static uint8_t line_obj_index[MAXW];
/* sprites the game shows every other frame (see flicker below): drawn apart, mixed half and half at the end */
static uint16_t line_ghost[MAXW];
static uint8_t line_ghost_prio[MAXW], line_ghost_index[MAXW];

static struct ss_view view;
static int page_shown;          /* a full-screen page over the field, in a wide view */
static int hud_at_edges;        /* this frame the field's status display goes to the edges of a wide view */

/* A page that fills the screen (status, items, the Start menu) is one composition with the status display and
 * everything stays in the middle then. Such a page crosses the column that parts the display's two groups in
 * the upper lines; a message window at the bottom does not count. */
static int page_open(void)
{
    int n, ty;
    for (n = 0; n < 4; n++) {
        uint16_t cnt = IO16(0x08 + n * 2);
        const uint16_t *map = (const uint16_t *)(VRAM + ((cnt >> 8) & 31) * 0x800);
        int scroll_x = IO16(0x10 + n * 4), scroll_y = IO16(0x12 + n * 4);
        if (view.bg[n].map || !(IO16(0) & (0x100 << n)))
            continue;
        for (ty = 0; ty < 13; ty++) {
            const uint16_t *row = map + (((ty * 8 + scroll_y) & 255) >> 3) * 32;
            if ((row[((HUD_SPLIT - 8 + scroll_x) & 255) >> 3] & 0x3FF) && (row[((HUD_SPLIT + scroll_x) & 255) >> 3] & 0x3FF))
                return 1;
        }
    }
    return 0;
}

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
    int X, split;

    if (source->map) {
        /* The console's own 240 columns are drawn from what the game put in the hardware (a menu or a window
         * drawn over the field is then always whole); the columns beside them come from the full source map:
         * world position, no wrapping. */
        int wy = y + source->scroll_y;
        for (X = 0; X < view.width; X++) {
            int wx = X - view.shift + source->scroll_x;
            if (X >= view.shift && X < view.shift + W) {
                int px = (X - view.shift + scroll_x) & (width - 1), py = (y + scroll_y) & (height - 1);
                int block = (px >> 8) + ((py >> 8) * (width == 512 ? 2 : 1));
                line_bg[n][X] = tile_pixel(chars, *(const uint16_t *)(map + block * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2),
                                           px & 7, py & 7, colour256);
                continue;
            }
            if (wx < 0 || wy < 0 || (wx >> 3) >= source->width || (wy >> 3) >= source->height)
                line_bg[n][X] = 0;
            else
                line_bg[n][X] = tile_pixel(chars, source->map[(wy >> 3) * source->width + (wx >> 3)], wx & 7, wy & 7, colour256);
        }
        return;
    }
    split = 0;
    if (hud_at_edges && y < HUD_BAND) {
        /* The field's status display: what is left of HUD_SPLIT stays at the left edge of the screen and what
         * is right of it goes to the right edge. A line where the layer crosses that column (a window, a
         * message) is one piece and stays in the middle. */
        int py = (y + scroll_y) & 255;
        const uint16_t *row = (const uint16_t *)(map + (py >> 3) * 64);
        split = !((row[((HUD_SPLIT - 8 + scroll_x) & 255) >> 3] & 0x3FF) && (row[((HUD_SPLIT + scroll_x) & 255) >> 3] & 0x3FF));
    }
    for (X = 0; X < view.width; X++) {
        int x = native_x(X, y), px, py, block;
        if (split)
            x = X < HUD_SPLIT ? X : (X >= view.width - W + HUD_SPLIT ? X - (view.width - W) : -1);
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

static const uint8_t obj_sizes[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

/* ---- flicker ----
 * The cartridge has one real transparency effect and many things to make transparent, so for shadows, ghosts,
 * the arrow that points to a companion and a character that has just been hurt it shows the sprite one frame
 * and hides it the next. On the console's slow screen that reads as half transparent; on a modern display it
 * is a hard blink, and when frames are not presented evenly (online, or a display that is not at 60 Hz) the
 * sprite can stay solid or vanish for a moment. So a sprite that alternates is recognised and drawn in every
 * frame at half strength instead, which is what the eye saw.
 *
 * Recognised from the last four frames of the sprite table: a sprite is one that alternates if it is here now,
 * was not in the frame before and was in the one before that; and one that is not here now is still shown
 * if it was in the last frame, in neither neighbour of that frame, and in the frame before those. A sprite that
 * simply appears, disappears or changes shape for an animation does not fit either pattern. Two entries are
 * the same sprite when they have the same size and priority and are within a few pixels. */
static uint16_t oam_history[4][128][3];
static uint8_t flicker_here[128], flicker_gone[128];
/* The sprite graphics and colours as they were when the last frame was drawn. The game hands out sprite tiles
 * anew every frame, so a sprite of the last frame that is drawn again in this one (flicker_gone) must take its
 * picture from then: the tiles it used may hold something else by now. */
static uint8_t past_tiles[0x8000];
static uint16_t past_colours[256];

static int obj_box(const uint16_t *e, int *x, int *y)
{
    if ((!(e[0] & 0x100) && (e[0] & 0x200)) || (e[0] >> 14) == 3)
        return 0;
    *y = e[0] & 255;
    if (*y >= 160) *y -= 256;
    *x = e[1] & 511;
    if (*x >= 384) *x -= 512;
    return *y > -64;
}

/* Where the camera was in each of those frames (the scroll of the field's map), so that a sprite of the world is
 * still recognised while the view moves: on the screen it shifts by what the camera moved. Sprites that do not
 * belong to the world (the status display) stay put, so both places are tried. */
static int camera_history[4][2];

static int obj_in(const uint16_t *e, int from, int in)
{
    uint16_t (*list)[3] = oam_history[in];
    int dx = camera_history[from][0] - camera_history[in][0], dy = camera_history[from][1] - camera_history[in][1];
    int x, y, x2, y2, j;
    if (!obj_box(e, &x, &y))
        return 0;
    for (j = 0; j < 128; j++) {
        const uint16_t *o = list[j];
        /* same shape, size, priority and palette: two different things the game alternates in one place (a
         * fallen character and its marker) are two sprites, each of them one that alternates */
        if (((o[0] ^ e[0]) & 0xC000) || ((o[1] ^ e[1]) & 0xC000) || ((o[2] ^ e[2]) & 0xFC00) || !obj_box(o, &x2, &y2))
            continue;
        if (x2 - x <= 8 && x - x2 <= 8 && y2 - y <= 8 && y - y2 <= 8)
            return 1;
        if (x2 - x - dx <= 8 && x + dx - x2 <= 8 && y2 - y - dy <= 8 && y + dy - y2 <= 8)
            return 1;
    }
    return 0;
}

static void find_flicker(void)
{
    int i, x, y;
    memmove(oam_history[1], oam_history[0], sizeof oam_history[0] * 3);
    memmove(camera_history[1], camera_history[0], sizeof camera_history[0] * 3);
    camera_history[0][0] = camera_history[0][1] = 0;
    for (i = 0; i < 4; i++)
        if (view.world && view.bg[i].map) {
            camera_history[0][0] = view.bg[i].scroll_x;
            camera_history[0][1] = view.bg[i].scroll_y;
            break;
        }
    for (i = 0; i < 128; i++) {
        oam_history[0][i][0] = OAM[i * 4];
        oam_history[0][i][1] = OAM[i * 4 + 1];
        oam_history[0][i][2] = OAM[i * 4 + 2];
    }
    for (i = 0; i < 128; i++) {
        const uint16_t *now = oam_history[0][i], *last = oam_history[1][i];
        flicker_here[i] = obj_box(now, &x, &y) && !obj_in(now, 0, 1) && obj_in(now, 0, 2);
        flicker_gone[i] = obj_box(last, &x, &y) && !obj_in(last, 1, 0) && !obj_in(last, 1, 2) && obj_in(last, 1, 3);
    }
}

/* One sprite on line y. `ghost`: it is one that alternates, and goes to the half-strength line; 2 if it is drawn
 * from the last frame's graphics. */
static void sprite(int y, uint16_t a0, uint16_t a1, uint16_t a2, int i, int ghost)
{
    const uint8_t *tiles = ghost == 2 ? past_tiles : VRAM + 0x10000;
    const uint16_t *colours = ghost == 2 ? past_colours : PAL + 0x100;
    uint16_t dispcnt = IO16(0);
    int one_dimensional = dispcnt & 0x40, extra = view.width - W;
    int affine = a0 & 0x100, mode = (a0 >> 10) & 3, shape = a0 >> 14;
    int width, height, box_w, box_h, x0, y0, row, colour256, tile, prio, bank, x, place;
    int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;

    if ((!affine && (a0 & 0x200)) || shape == 3)
        return;
    width = obj_sizes[shape][a1 >> 14][0];
    height = obj_sizes[shape][a1 >> 14][1];
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
        return;
    x0 = a1 & 511;
    /* On the console anything from 240 up is off screen or a wrapped negative position. With a wider view a
     * sprite just right of 240 is visible, so only the top of the range is read as negative. */
    if (x0 >= (extra ? 512 - 128 : 240)) x0 -= 512;
    /* where this sprite goes in the view: sprites of a scene with a source map with the world, the icons of
     * the status display to their edge, anything else centred */
    place = view.world ? view.shift : extra / 2;
    colour256 = a0 & 0x2000;
    tile = a2 & 0x3FF;
    prio = (a2 >> 10) & 3;
    if (hud_at_edges && prio == 0 && y0 >= 0 && y0 + box_h <= HUD_BAND) {
        int centre = x0 + box_w / 2;
        if (centre < 72) place = 0;
        else if (centre >= HUD_SPLIT) place = extra;
    }
    bank = a2 >> 12;
    if (affine) {
        const uint16_t *matrix = OAM + ((a1 >> 9) & 31) * 16;
        pa = (int16_t)matrix[3]; pb = (int16_t)matrix[7]; pc = (int16_t)matrix[11]; pd = (int16_t)matrix[15];
    }
    for (x = 0; x < box_w; x++) {
        int sx = x0 + x + place, tx, ty, number, index;
        if (sx < 0 || sx >= view.width)
            continue;
        if (page_shown && (sx < view.shift || sx >= view.shift + W))
            continue;                   /* a page is the console's picture: nothing of it belongs beside it */
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
            index = tiles[(number & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7)];
        } else {
            uint8_t byte;
            number = tile + (ty >> 3) * (one_dimensional ? (width >> 3) : 32) + (tx >> 3);
            byte = tiles[(number & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)];
            index = (tx & 1) ? byte >> 4 : byte & 15;
            if (index) index += bank * 16;
        }
        if (!index)
            continue;
        if (mode == 2) {
            line_obj_flags[sx] |= 2;
            continue;
        }
        if (ghost) {
            if (prio <= line_ghost_prio[sx] || !line_ghost[sx]) {
                line_ghost[sx] = (uint16_t)(0x8000 | (colours[index] & 0x7FFF));       /* the colour itself */
                line_ghost_prio[sx] = (uint8_t)prio;
                line_ghost_index[sx] = (uint8_t)i;
            }
        } else if (prio <= line_obj_prio[sx] || !line_obj[sx]) {
            line_obj[sx] = (uint16_t)(0x100 + index + 1);
            line_obj_prio[sx] = (uint8_t)prio;
            line_obj_index[sx] = (uint8_t)i;
            line_obj_flags[sx] = (uint8_t)((line_obj_flags[sx] & 2) | (mode == 1));
        }
    }
}

static void sprites(int y)
{
    int i;
    memset(line_obj, 0, sizeof line_obj);
    memset(line_obj_prio, 4, sizeof line_obj_prio);
    memset(line_obj_flags, 0, sizeof line_obj_flags);
    memset(line_ghost, 0, sizeof line_ghost);
    for (i = 127; i >= 0; i--) {
        if (flicker_gone[i]) {
            /* where it would be now: a sprite of the world moved on the screen by what the camera moved */
            uint16_t a0 = oam_history[1][i][0], a1 = oam_history[1][i][1];
            int moved_x = camera_history[1][0] - camera_history[0][0], moved_y = camera_history[1][1] - camera_history[0][1];
            if (((oam_history[1][i][2] >> 10) & 3) != 0 && moved_x > -16 && moved_x < 16 && moved_y > -16 && moved_y < 16) {
                a0 = (uint16_t)((a0 & 0xFF00) | ((a0 + moved_y) & 0xFF));
                a1 = (uint16_t)((a1 & 0xFE00) | ((a1 + moved_x) & 0x1FF));
            }
            sprite(y, a0, a1, oam_history[1][i][2], i, 2);
        }
        sprite(y, OAM[i * 4], OAM[i * 4 + 1], OAM[i * 4 + 2], i, flicker_here[i]);
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
    uint16_t dispcnt, bldcnt;
    int mode, eva, evb, evy, windows, y, X, n;
    int prio[4], enabled[4];
    uint8_t end_of_frame[SS_LINE_REGISTERS];
    int per_line = ss_line_registers(0) != NULL, edge_fade;

    if (requested) {
        view = *requested;
    } else {
        memset(&view, 0, sizeof view);
        view.width = W;
    }
    if (!view.hud_right) view.hud_right = view.width;
    if (view.width < W) view.width = W;
    if (view.width > MAXW) view.width = MAXW;
    memcpy(end_of_frame, (const void *)(uintptr_t)SS_IO, sizeof end_of_frame);
    find_flicker();
    edge_fade = view.world && (view.hud_left > 0 || view.hud_right < view.width);
    page_shown = view.world && view.width > W && (IO16(0) & 7) == 0 && page_open();
    hud_at_edges = view.world && view.width > W && (IO16(0) & 7) == 0 && !page_shown;
    for (y = 0; y < H; y++) {
        uint32_t *out = pixels + y * pitch_pixels;
        if (per_line)
            memcpy((void *)(uintptr_t)SS_IO, ss_line_registers(y), SS_LINE_REGISTERS);
        if (y == 0 || per_line) {
            dispcnt = IO16(0); bldcnt = IO16(0x50);
            mode = dispcnt & 7; eva = IO16(0x52) & 31; evb = (IO16(0x52) >> 8) & 31; evy = IO16(0x54) & 31;
            windows = dispcnt & 0xE000;
            if (eva > 16) eva = 16;
            if (evb > 16) evb = 16;
            if (evy > 16) evy = 16;
            for (n = 0; n < 4; n++) {
                prio[n] = IO16(0x08 + n * 2) & 3;
                enabled[n] = (dispcnt & (0x100 << n)) && ((mode == 0) || (mode == 1 && n < 3) || (mode == 2 && n >= 2));
            }
        }
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
                out[X] = 0x00000000u;     /* transparent: the platform shows its own field there */       /* side bars of a scene that keeps the console's composition */
                continue;
            }
            if (view.world && (X < view.hud_left || X >= view.hud_right))
                top_colour = second_colour = 0;     /* beside a map narrower than the view there is nothing: black, which continues the dark around the map */
            if (windows) {
                /* window coordinates are native; the columns beyond the console's screen take the edge value, so a
                 * full-screen mask (the scene-change blackout) covers them too */
                int x = X - (view.width - W) / 2;      /* the console's columns are always the middle ones */
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
            if (line_ghost[X] && (dispcnt & 0x1000) && (mask & 0x10)) {
                /* a sprite that alternates, where it is in front of what was drawn: half of each */
                int g = line_ghost_prio[X], front;
                if (top == LAYER_OBJ)
                    front = g < line_obj_prio[X] || (g == line_obj_prio[X] && line_ghost_index[X] < line_obj_index[X]);
                else
                    front = top == LAYER_BACKDROP || g <= prio[top];
                if (front) {
                    uint16_t colour = line_ghost[X] & 0x7FFF;
                    if ((mask & 0x20) && (bldcnt & 0x10) && ((bldcnt >> 6) & 3) >= 2)
                        colour = fade(colour, evy, ((bldcnt >> 6) & 3) == 2);
                    top_colour = blend(top_colour, colour, 8, 8);
                }
            }
            if (edge_fade && (top == LAYER_BACKDROP || (top == LAYER_OBJ ? line_obj_prio[X] > 0 : view.bg[top].map != NULL))) {
                /* the last columns of a map narrower than the view sink into the black beside it */
                int from_left = X - view.hud_left, from_right = view.hud_right - 1 - X, by = -1;
                if (view.hud_left > 0 && from_left >= 0 && from_left < EDGE_FADE) by = from_left;
                if (view.hud_right < view.width && from_right >= 0 && from_right < EDGE_FADE) by = from_right;
                if (by >= 0)
                    top_colour = fade(top_colour, 16 - (by + 1) * 16 / (EDGE_FADE + 1), 0);
            }
            out[X] = to_rgb(top_colour);
        }
    }
    if (per_line)
        memcpy((void *)(uintptr_t)SS_IO, end_of_frame, sizeof end_of_frame);
    memcpy(past_tiles, VRAM + 0x10000, sizeof past_tiles);
    memcpy(past_colours, PAL + 0x100, sizeof past_colours);
}
