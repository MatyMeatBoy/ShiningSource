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

#include <stdlib.h>
#include <string.h>

#define IO16(o)  (*(const uint16_t *)(uintptr_t)(SS_IO + (o)))
#define IO32(o)  (*(const uint32_t *)(uintptr_t)(SS_IO + (o)))
#define PAL      ((const uint16_t *)(uintptr_t)SS_PALETTE)
#define VRAM     ((const uint8_t *)(uintptr_t)SS_VRAM)
#define OAM      ((const uint16_t *)(uintptr_t)SS_OAM)

/* the field's status display in a wide view: its lines, and the column that parts its left and right groups */
enum { HUD_BAND = 56, HUD_SPLIT = 176, EDGE_FADE = 32 };
enum { W = SS_SCREEN_W, H = SS_SCREEN_H, MAXW = SS_MAX_VIEW_W, LAYER_OBJ = 4, LAYER_BACKDROP = 5 };

/* per scanline, per background: palette index + 1 (0 = transparent); the sprites have their own lines further down */
static uint16_t line_bg[4][MAXW];

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

/* `run` pixels of one tile, from its column `tx` on, of its row `ty`: what tile_pixel gives for each, with the
 * tile's row found once. An empty row of a 16-colour tile, by far the most common, is written in one go. */
static void tile_run(uint16_t *dst, const uint8_t *chars, uint16_t entry, int tx, int ty, int run, int colour256)
{
    int i;
    if (entry & 0x800) ty = 7 - ty;
    if (colour256) {
        const uint8_t *src = chars + (entry & 0x3FF) * 64 + ty * 8;
        for (i = 0; i < run; i++) {
            int index = src[(entry & 0x400) ? 7 - (tx + i) : tx + i];
            dst[i] = (uint16_t)(index ? index + 1 : 0);
        }
    } else {
        uint32_t bits;
        int base = (entry >> 12) * 16 + 1;
        memcpy(&bits, chars + (entry & 0x3FF) * 32 + ty * 4, 4);
        if (!bits) {
            for (i = 0; i < run; i++) dst[i] = 0;
            return;
        }
        if (entry & 0x400) {            /* mirrored: the eight nibbles in the opposite order */
            bits = (bits >> 16) | (bits << 16);
            bits = ((bits >> 8) & 0x00FF00FFu) | ((bits << 8) & 0xFF00FF00u);
            bits = ((bits >> 4) & 0x0F0F0F0Fu) | ((bits << 4) & 0xF0F0F0F0u);
        }
        bits >>= tx * 4;
        for (i = 0; i < run; i++, bits >>= 4) {
            int index = bits & 15;
            dst[i] = (uint16_t)(index ? base + index : 0);
        }
    }
}

/* `count` pixels of a background as the hardware holds it, from its pixel (px, py) on, wrapping as it does. */
static void hardware_span(uint16_t *dst, int count, int px, int py, const uint8_t *chars, const uint8_t *map,
                          int width, int height, int colour256)
{
    const uint8_t *row;
    py &= height - 1;
    row = map + (py >> 8) * (width == 512 ? 2 : 1) * 0x800 + ((py & 255) >> 3) * 64;
    while (count > 0) {
        int tx, run;
        px &= width - 1;
        tx = px & 7;
        run = 8 - tx < count ? 8 - tx : count;
        tile_run(dst, chars, *(const uint16_t *)(row + (px >> 8) * 0x800 + ((px & 255) >> 3) * 2), tx, py & 7, run, colour256);
        dst += run; px += run; count -= run;
    }
}

/* `count` pixels of a background from its full source map, from world position (wx, wy) on: no wrapping, and
 * nothing outside the map. */
static void source_span(uint16_t *dst, int count, int wx, int wy, const struct ss_bg_source *source, const uint8_t *chars, int colour256)
{
    int inside_rows = wy >= 0 && (wy >> 3) < source->height;
    while (count > 0) {
        int tx, run;
        if (!inside_rows || wx < 0 || (wx >> 3) >= source->width) {
            run = (inside_rows && wx < 0 && -wx < count) ? -wx : count;
            memset(dst, 0, (size_t)run * sizeof *dst);
        } else {
            tx = wx & 7;
            run = 8 - tx < count ? 8 - tx : count;
            tile_run(dst, chars, source->map[(wy >> 3) * source->width + (wx >> 3)], tx, wy & 7, run, colour256);
        }
        dst += run; wx += run; count -= run;
    }
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
    uint16_t *dst = line_bg[n];
    int X, split;

    if (source->map) {
        /* The console's own 240 columns are drawn from what the game put in the hardware (a menu or a window
         * drawn over the field is then always whole); the columns beside them come from the full source map:
         * world position, no wrapping. */
        int wy = y + source->scroll_y;
        int from = view.shift < 0 ? 0 : view.shift > view.width ? view.width : view.shift;
        int to = view.shift + W < from ? from : view.shift + W > view.width ? view.width : view.shift + W;
        if (from > 0)
            source_span(dst, from, source->scroll_x - view.shift, wy, source, chars, colour256);
        if (to > from)
            hardware_span(dst + from, to - from, from - view.shift + scroll_x, y + scroll_y, chars, map, width, height, colour256);
        if (to < view.width)
            source_span(dst + to, view.width - to, to - view.shift + source->scroll_x, wy, source, chars, colour256);
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
    for (X = 0; X < view.width; ) {
        /* the native column shown here, and for how many columns the ones after it follow on */
#define NATIVE_AT(X) (split ? ((X) < HUD_SPLIT ? (X) : ((X) >= view.width - W + HUD_SPLIT ? (X) - (view.width - W) : -1)) : native_x((X), y))
        int x = NATIVE_AT(X), run = 1;
        if (x < 0) {
            dst[X++] = 0;
            continue;
        }
        while (X + run < view.width && NATIVE_AT(X + run) == x + run)
            run++;
#undef NATIVE_AT
        hardware_span(dst + X, run, x + scroll_x, y + scroll_y, chars, map, width, height, colour256);
        X += run;
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

/* ---- sprites that alternate ----
 * The cartridge has one real transparency effect and many things to make transparent, so for shadows, ghosts,
 * the arrow that points to a companion, the labels over companions and a character that has just been hurt it
 * shows the sprite one frame and hides it the next (some effects: two frames shown, two hidden), or shows two
 * things in one place in turns. On the console's slow screen that reads as half transparent; on a modern
 * display it is a hard blink, and when frames are not presented evenly (online, or a display that is not at
 * 60 Hz) the sprite can stay solid or vanish for a moment.
 *
 * What the slow screen showed is the average of two pictures: this frame, and the frame that goes with it, in
 * which the things that alternate have changed places: those shown now are hidden, those hidden now are shown.
 * So that is what is drawn. Where something alternates, the pixel is composed twice, once with the sprites of
 * this frame and once with the sprites of the other frame, each time in full (priorities, windows, blending,
 * fades), and the two results are averaged. A sprite that alternates with nothing comes out at half strength
 * over whatever is behind it; two that take turns come out as half of each; one hidden behind a solid sprite
 * stays hidden. Everything else is the same in both pictures and is composed once, as the console draws it.
 *
 * Which sprites alternate is read from the sprite table of the last frames. The game builds that table anew
 * every frame, and hands out the sprite tiles anew too, so neither the entry number nor the tile number says
 * which sprite is which. A sprite is known by what it looks like (its tiles' contents, with its shape, size,
 * palette and priority) and by where it is: each one seen is followed from frame to frame (`tracks`), across
 * the camera's movement and its own, and a record is kept of the frames it was in. One that is here now,
 * after a gap of one or two frames, having been here before the gap for no longer than the gap, alternates;
 * so does one that is missing now for no longer than its last gap, with turns as short before it. A sprite
 * that simply appears, disappears, blinks slowly, drops out for a frame or changes its picture for an
 * animation never shows that pattern and is drawn as it is. The pattern needs two turns to show: a sprite that
 * starts to alternate blinks once (one that was there all along: twice) before it is steady.
 *
 * A sprite that is in the other frame only is drawn from the frame it was last seen in: its entry, its tiles
 * and its colours as they were then (the tiles may hold something else by now), placed where it would be now:
 * moved on by the camera and by its own speed. When the game stops alternating a sprite, the copy is drawn
 * for the one frame in which that cannot be known yet (two frames, for two shown and two hidden).
 * SS_FLICKER=0 turns all of this off: every frame is drawn as the console draws it. */
enum { TRACKS = 192, KEPT = 3, COPIES = 64, REACH = 4 /* pixels a frame a sprite may move and still be followed */ };

struct obj_line {
    uint16_t colour[MAXW];      /* 0x101 + palette index, or 0x8000 + the colour itself (a copy from a past frame); 0: nothing */
    uint8_t prio[MAXW];
    uint8_t flags[MAXW];        /* 1 semi-transparent, 2 sprite-window pixel, 4 the sprite is not where it last was */
};
static struct obj_line obj_now, obj_other;      /* the sprites of this frame, and of the frame that goes with it */

struct track {
    uint32_t content;           /* what it looks like: a sum over its tiles */
    uint32_t born, last, before;    /* frame it was first seen in, last seen in, and seen in before that */
    uint16_t kind;              /* shape, size, colour mode, priority, palette */
    int16_t x, y;               /* where on the screen it was when last seen */
    int16_t camera_x, camera_y; /* where the camera was then */
    int16_t move_x, move_y;     /* how far it moved between its last two sightings (in the world) ... */
    uint8_t steps;              /* ... which were this many frames apart (0: seen once) */
    uint8_t fixed;              /* it keeps its place on the screen while the camera moves (the status display) */
    uint16_t seen;              /* bit 0: it is in this frame, bit 1: in the one before, ...; 0: record not in use */
    uint8_t entry, kept;        /* its sprite table entry when last seen, and which kept frame that is */
};
static struct track tracks[TRACKS];
/* the sprite table, sprite tiles and sprite colours of the last frames, to draw from */
static struct { uint16_t oam[512]; uint8_t tiles[0x8000]; uint16_t colours[256]; } kept[KEPT];
static uint32_t frame_drawn;
static int frame_known, frame_is_new;
static int camera_x, camera_y;

static uint8_t only_now[128];       /* this entry alternates: it belongs to this frame's picture only */
static uint8_t obj_moved[128];      /* this entry is not where it was last seen (or was never seen) */
static struct { uint16_t entry[3]; uint8_t index, kept, moved; } copies[COPIES];   /* sprites of the other frame's picture only */
static int copy_count;
static uint8_t two_pictures[H];     /* lines on which something alternates */

/* Per pixel of the last picture: 1 where what is shown has not moved since the frame before (a background
 * that did not scroll, a sprite that stayed in its place). The smoothing of blinking in frame.c works only
 * there: anything that moves must stay sharp. Row y starts at y * the picture's width. */
static uint8_t still[MAXW * H];

const uint8_t *ss_ppu_still(void)
{
    return still;
}

static int obj_box(const uint16_t *e, int *x, int *y, int *w, int *h)
{
    if ((!(e[0] & 0x100) && (e[0] & 0x200)) || (e[0] >> 14) == 3)
        return 0;
    *y = e[0] & 255;
    if (*y >= 160) *y -= 256;
    *x = e[1] & 511;
    if (*x >= 384) *x -= 512;
    *w = obj_sizes[e[0] >> 14][e[1] >> 14][0];
    *h = obj_sizes[e[0] >> 14][e[1] >> 14][1];
    return *y > -64;
}

/* What a sprite looks like, whatever tiles it was given this frame: a sum over the contents of its tiles.
 * 0 if it is empty. */
static uint32_t obj_content(const uint16_t *e, int w, int h)
{
    const uint8_t *tiles = VRAM + 0x10000;
    int slots = (e[0] & 0x2000) ? 2 : 1, per_row = (w >> 3) * slots, rows = h >> 3;
    int step = (IO16(0) & 0x40) ? per_row : 32, r, t, k;
    uint32_t sum = 0x9E3779B1u, any = 0;
    for (r = 0; r < rows; r++) {
        for (t = 0; t < per_row; t++) {
            const uint32_t *words = (const uint32_t *)(tiles + (((e[2] & 0x3FF) + r * step + t) & 0x3FF) * 32);
            for (k = 0; k < 8; k++) {
                any |= words[k];
                sum = (sum ^ words[k]) * 0x01000193u;
                sum ^= sum >> 15;
            }
        }
    }
    return any ? (sum | 1) : 0;
}

static int apart(int dx, int dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}

/* n / d rounded to the nearest whole number; a half goes towards zero: where a sprite's step cannot be told
 * to the pixel, it is taken to have moved the smaller amount */
static int nearest(int n, int d)
{
    return n >= 0 ? (2 * n + d - 1) / (2 * d) : -((-2 * n + d - 1) / (2 * d));
}

/* Where a sprite last seen `frames` ago would be now: carried along by the camera unless it belongs to the
 * screen, and moved on at its own speed. */
static void track_place(const struct track *t, int frames, int *x, int *y)
{
    *x = t->x;
    *y = t->y;
    if (t->fixed)
        return;
    *x -= camera_x - t->camera_x;
    *y -= camera_y - t->camera_y;
    if (t->steps) {
        *x += nearest(t->move_x * frames, t->steps);
        *y += nearest(t->move_y * frames, t->steps);
    }
}

/* The record of the frames a sprite was in, read as runs: `seen` starts with a run of frames it was in (or
 * was not in), then a run of the opposite, and so on. */
static int run(unsigned seen, int *from, int value)
{
    int length = 0;
    while (*from < 16 && (int)((seen >> *from) & 1) == value) {
        (*from)++;
        length++;
    }
    return length;
}

/* Here now, after a gap of one or two frames, and before the gap it was here for no longer than the gap; and
 * it has not been here since for longer than the gap either (then it has come to stay). */
static int alternates_here(unsigned seen)
{
    int from = 0, here = run(seen, &from, 1), gap = run(seen, &from, 0), earlier = run(seen, &from, 1);
    return here >= 1 && gap >= 1 && gap <= 2 && here <= gap && earlier >= 1 && earlier <= gap;
}

/* Not here now, but it alternates and this is its turn to be hidden: it was here until one or two frames ago,
 * for no longer than the gap before that, it has not been missing for longer than that gap, and before that gap
 * it was not here for longer either. (A sprite that was simply there, went away for a moment, came back and
 * left again is not one that alternates.) */
static int alternates_away(unsigned seen)
{
    int from = 0, away = run(seen, &from, 0), here = run(seen, &from, 1), gap = run(seen, &from, 0), earlier = run(seen, &from, 1);
    return away >= 1 && here >= 1 && gap >= 1 && gap <= 2 && away <= gap && here <= gap && earlier >= 1 && earlier <= gap;
}

struct seen_obj { uint32_t content; uint16_t kind; int16_t x, y; int16_t track; };

/* The record that fits a sprite best, or -1: one that looks the same (`same`), or else one of the same kind
 * with another picture that was not in the last frame (the sprite went on to its next animation picture while
 * it was hidden: a hurt character keeps walking). It must be within reach of where the record was last seen;
 * the nearest to where the record would be by now wins. */
static int best_track(const struct seen_obj *s, int same, int *score)
{
    int best = -1, best_score = 0, best_age = 0, n;
    for (n = 0; n < TRACKS; n++) {
        const struct track *t = &tracks[n];
        int age, reach, x, y, d;
        if (!t->seen || (t->seen & 1) || t->kind != s->kind || (t->content == s->content) != same)
            continue;
        if (!same && (t->seen & 2))
            continue;                   /* (in the last frame: then this is its next picture, or another sprite) */
        age = (int)(frame_drawn - t->last);
        reach = REACH * age + 1;
        /* within reach of where it was: on the screen, or in the world */
        if (apart(s->x - t->x, s->y - t->y) > reach
            && apart(s->x - t->x + (camera_x - t->camera_x), s->y - t->y + (camera_y - t->camera_y)) > reach)
            continue;
        track_place(t, age, &x, &y);
        d = apart(s->x - x, s->y - y);
        if (best < 0 || d < best_score || (d == best_score && age < best_age)) {
            best = n;
            best_score = d;
            best_age = age;
        }
    }
    *score = best_score > 31 ? 31 : best_score;
    return best;
}

static void track_seen(struct track *t, struct seen_obj *s, int entry)
{
    int age = (int)(frame_drawn - t->last);
    int dx = s->x - t->x, dy = s->y - t->y;
    int cx = camera_x - t->camera_x, cy = camera_y - t->camera_y;
    if (dx || dy) {
        t->fixed = 0;
        t->move_x = (int16_t)(dx + cx);
        t->move_y = (int16_t)(dy + cy);
    } else {
        if (cx || cy)
            t->fixed = 1;               /* the camera moved and it did not: it belongs to the screen */
        t->move_x = t->move_y = 0;
    }
    t->steps = (uint8_t)age;
    t->x = s->x; t->y = s->y;
    t->camera_x = (int16_t)camera_x; t->camera_y = (int16_t)camera_y;
    t->before = t->last;
    t->last = frame_drawn;
    t->content = s->content;
    t->entry = (uint8_t)entry;
    t->kept = (uint8_t)(frame_drawn % KEPT);
    t->seen |= 1;
    s->track = (int16_t)(t - tracks);
    obj_moved[entry] = (uint8_t)(dx || dy);
}

static void mark_lines(const uint16_t *e)
{
    int x, y, w, h;
    if (!obj_box(e, &x, &y, &w, &h))
        return;
    if ((e[0] & 0x300) == 0x300)
        h *= 2;
    for (h += y; y < h; y++)
        if (y >= 0 && y < H)
            two_pictures[y] = 1;
}

static void find_alternating(void)
{
    static struct seen_obj seen[128];
    static uint8_t due_entry[TRACKS], due_kept[TRACKS];
    static uint16_t due[TRACKS];
    static int16_t due_x[TRACKS], due_y[TRACKS];
    static int enabled = -1;
    uint8_t order[32][128], counts[32];
    uint32_t frame = (uint32_t)ss_frame_number();
    int i, n, score;

    if (enabled < 0)
        enabled = !(getenv("SS_FLICKER") && getenv("SS_FLICKER")[0] == '0');
    frame_is_new = !frame_known || frame != frame_drawn;
    if (!frame_is_new)
        return;                         /* the same frame drawn again (a screenshot): nothing has changed */
    if (!frame_known || frame != frame_drawn + 1)
        memset(tracks, 0, sizeof tracks);       /* frames were left out (a skipped scene): start afresh */
    frame_known = 1;
    frame_drawn = frame;
    /* where the camera is (the scroll of the field's map): a sprite of the world moves on the screen by what the
     * camera moved */
    camera_x = camera_y = 0;
    for (i = 0; i < 4; i++)
        if (view.world && view.bg[i].map) {
            camera_x = view.bg[i].scroll_x;
            camera_y = view.bg[i].scroll_y;
            break;
        }
    memset(only_now, 0, sizeof only_now);
    memset(obj_moved, 1, sizeof obj_moved);
    memset(two_pictures, 0, sizeof two_pictures);
    copy_count = 0;
    for (n = 0; n < TRACKS; n++) {
        struct track *t = &tracks[n];
        t->seen = (uint16_t)(t->seen << 1);
        if (!(t->seen & 0x0E))
            t->seen = 0;                /* not seen for three frames: forgotten */
        /* those that alternate, were hidden in the last frame and are due back in this one: where, and their record */
        due[n] = 0;
        if (t->seen && alternates_away((unsigned)t->seen >> 1)) {
            int x, y;
            track_place(t, (int)(frame - t->last), &x, &y);
            due[n] = t->seen;
            due_x[n] = (int16_t)x;
            due_y[n] = (int16_t)y;
            due_entry[n] = t->entry;
            due_kept[n] = t->kept;
        }
    }

    /* the sprites of this frame, each with the record that fits it best */
    memset(counts, 0, sizeof counts);
    for (i = 0; i < 128; i++) {
        const uint16_t *e = OAM + i * 4;
        struct seen_obj *s = &seen[i];
        int x, y, w, h;
        s->track = -1;
        s->content = 0;
        if (!(IO16(0) & 0x1000) || !obj_box(e, &x, &y, &w, &h) || !(s->content = obj_content(e, w, h)))
            continue;
        s->kind = (uint16_t)((e[0] >> 8) | (e[1] >> 14) << 8 | (e[2] >> 10) << 10);
        s->x = (int16_t)x;
        s->y = (int16_t)y;
        if (best_track(s, 1, &score) >= 0)
            order[score][counts[score]++] = (uint8_t)i;
    }
    /* the closest fits first, so that of two sprites that look alike each gets its own record */
    for (score = 0; score < 32; score++)
        for (n = 0; n < counts[score]; n++) {
            int found, unused;
            i = order[score][n];
            found = best_track(&seen[i], 1, &unused);
            if (found >= 0)
                track_seen(&tracks[found], &seen[i], i);
        }
    for (i = 0; i < 128; i++) {
        struct seen_obj *s = &seen[i];
        int found, earlier = -1;
        if (!s->content || s->track >= 0)
            continue;
        /* A picture not seen before. If a sprite of this kind was close by in the last frame and is not there
         * now, this is its next animation picture (or the thing it takes turns with): a new record, which starts
         * with that sprite's record of frames, so that a sprite that only changes its picture counts as having
         * been here all along. Else it may be a hidden sprite that came back with its next picture. */
        for (n = 0; n < TRACKS; n++)
            if ((tracks[n].seen & 3) == 2 && tracks[n].kind == s->kind && apart(s->x - tracks[n].x, s->y - tracks[n].y) <= 16
                && (earlier < 0 || apart(s->x - tracks[n].x, s->y - tracks[n].y) < apart(s->x - tracks[earlier].x, s->y - tracks[earlier].y)))
                earlier = n;
        found = earlier < 0 ? best_track(s, 0, &score) : -1;
        if (found >= 0) {               /* a hidden sprite came back with its next animation picture */
            track_seen(&tracks[found], s, i);
            continue;
        }
        for (n = 0; n < TRACKS && tracks[n].seen; n++)
            ;
        if (n < TRACKS) {               /* a sprite not seen before (when all records are in use it is simply drawn) */
            struct track *t = &tracks[n];
            unsigned record = earlier >= 0 ? tracks[earlier].seen | 1u : 1;
            int x, y, w, h, u;
            /* A character is made of several sprites, and its next animation picture can be cut into pieces of
             * other sizes. A new sprite that lies over an alternating one that is due back now, with the same
             * palette and priority, is the next piece of the same thing: it takes over that record of frames. */
            obj_box(OAM + i * 4, &x, &y, &w, &h);
            for (u = 0; u < TRACKS && earlier < 0 && record == 1; u++) {
                int ux, uy, uw, uh;
                if (!due[u] || ((tracks[u].kind ^ s->kind) & 0xFC00) || !obj_box(kept[due_kept[u]].oam + due_entry[u] * 4, &ux, &uy, &uw, &uh))
                    continue;
                ux = due_x[u];
                uy = due_y[u];
                if (x < ux + uw + 4 && ux < x + w + 4 && y < uy + uh + 4 && uy < y + h + 4)
                    record = due[u] | 1u;
            }
            memset(t, 0, sizeof *t);
            t->content = s->content;
            t->kind = s->kind;
            t->born = t->last = t->before = frame;
            t->x = s->x; t->y = s->y;
            t->camera_x = (int16_t)camera_x; t->camera_y = (int16_t)camera_y;
            t->entry = (uint8_t)i;
            t->kept = (uint8_t)(frame % KEPT);
            t->seen = (uint16_t)record;
            s->track = (int16_t)n;
        }
    }
    /* A sprite that is back where it was two frames ago has not alternated if a sprite of its kind turned up
     * close by in between and is gone again now: that is one sprite going there and back (bouncing off a wall,
     * shaking, a lunge in an attack animation), and it must stay solid. Its record starts again. (Two things
     * that really take turns close to each other pass this the second time round: by then both are known.) */
    for (n = 0; n < TRACKS; n++) {
        struct track *t = &tracks[n];
        if ((t->seen & 3) != 1 || t->seen == 1)
            continue;
        for (i = 0; i < TRACKS; i++) {
            const struct track *u = &tracks[i];
            if (u->seen && !(u->seen & 1) && u != t && u->kind == t->kind && u->born > t->before
                && apart(u->x - t->x, u->y - t->y) <= 16 * (int)(frame - t->before)) {
                t->seen = 1;
                break;
            }
        }
    }
    if (!enabled)
        return;
    for (n = 0; n < TRACKS; n++) {
        const struct track *t = &tracks[n];
        if (!t->seen)
            continue;
        if (t->seen & 1) {
            if (alternates_here(t->seen)) {
                only_now[t->entry] = 1;
                mark_lines(OAM + t->entry * 4);
            }
        } else if (alternates_away(t->seen) && copy_count < COPIES) {
            const uint16_t *e = kept[t->kept].oam + t->entry * 4;
            int x, y;
            track_place(t, (int)(frame - t->last), &x, &y);
            if (x < -128 || x >= 384 || y <= -64 || y >= 160)
                continue;
            copies[copy_count].entry[0] = (uint16_t)((e[0] & 0xFF00) | (y & 0xFF));
            copies[copy_count].entry[1] = (uint16_t)((e[1] & 0xFE00) | (x & 0x1FF));
            copies[copy_count].entry[2] = e[2];
            copies[copy_count].index = t->entry;
            copies[copy_count].kept = t->kept;
            copies[copy_count].moved = (uint8_t)(x != t->x || y != t->y);
            mark_lines(copies[copy_count].entry);
            copy_count++;
        }
    }
}

/* One sprite on line y, into `line`. `oam` is the sprite table it belongs to (for its rotation), `tiles` its
 * graphics, `colours` its palette if it is drawn from a past frame (NULL: this frame's, looked up later). */
static void sprite(int y, const uint16_t *e, const uint16_t *oam, const uint8_t *tiles, const uint16_t *colours, int moved, struct obj_line *line)
{
    uint16_t a0 = e[0], a1 = e[1], a2 = e[2];
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
        const uint16_t *matrix = oam + ((a1 >> 9) & 31) * 16;
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
            line->flags[sx] |= 2;
            continue;
        }
        if (prio <= line->prio[sx] || !line->colour[sx]) {
            line->colour[sx] = colours ? (uint16_t)(0x8000 | (colours[index] & 0x7FFF)) : (uint16_t)(0x100 + index + 1);
            line->prio[sx] = (uint8_t)prio;
            line->flags[sx] = (uint8_t)((line->flags[sx] & 2) | (mode == 1) | moved);
        }
    }
}

static void sprites(int y, int two)
{
    int i, n;
    memset(obj_now.colour, 0, sizeof obj_now.colour);
    memset(obj_now.prio, 4, sizeof obj_now.prio);
    memset(obj_now.flags, 0, sizeof obj_now.flags);
    for (i = 127; i >= 0; i--)
        sprite(y, OAM + i * 4, OAM, VRAM + 0x10000, NULL, obj_moved[i] ? 4 : 0, &obj_now);
    if (!two)
        return;
    /* The other frame's sprites: this frame's without those that alternate, and those that are hidden now, each
     * in the place it had in its own sprite table. The game fills the table without gaps, so the entries behind
     * a hidden sprite have moved up by one: the hidden sprite goes in front of the entry that has its number now. */
    memset(obj_other.colour, 0, sizeof obj_other.colour);
    memset(obj_other.prio, 4, sizeof obj_other.prio);
    memset(obj_other.flags, 0, sizeof obj_other.flags);
    for (i = 127; i >= 0; i--) {
        if (!only_now[i])
            sprite(y, OAM + i * 4, OAM, VRAM + 0x10000, NULL, obj_moved[i] ? 4 : 0, &obj_other);
        for (n = 0; n < copy_count; n++)
            if (copies[n].index == i)
                sprite(y, copies[n].entry, kept[copies[n].kept].oam, kept[copies[n].kept].tiles, kept[copies[n].kept].colours,
                       copies[n].moved ? 4 : 0, &obj_other);
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

/* the registers of the line being drawn */
static struct {
    uint16_t dispcnt, bldcnt;
    int eva, evb, evy, windows;
    int prio[4], enabled[4];
    int order[4], order_prio[4], count;     /* the enabled backgrounds from the back to the front, and their priorities */
    int moved[4];               /* this background is not where it was on this line in the last frame */
    int edge_fade;
} line;

/* One pixel of the picture with the sprites of `obj`: the layers in their order, the windows, the blending and
 * the fades. `moved` tells whether what is shown in front moved since the last frame. */
static uint16_t compose(int X, int y, const struct obj_line *obj, int *moved)
{
    int mask = 0x3F, top = LAYER_BACKDROP, second = LAYER_BACKDROP, p, n;
    uint16_t dispcnt = line.dispcnt, bldcnt = line.bldcnt;
    uint16_t top_colour = PAL[0], second_colour = PAL[0];
    int obj_visible, semi = 0;

    if (view.world && (X < view.hud_left || X >= view.hud_right))
        top_colour = second_colour = 0;     /* beside a map narrower than the view there is nothing: black, which continues the dark around the map */
    if (line.windows) {
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
        else if ((dispcnt & 0x8000) && (dispcnt & 0x1000) && (obj->flags[X] & 2))
            mask = (winout >> 8) & 0x3F;
        else
            mask = winout & 0x3F;
    }
    obj_visible = (dispcnt & 0x1000) && (mask & 0x10) && obj->colour[X];
    /* The front-most pixel: the layers from the front, the sprite in front of the backgrounds of its own
     * priority, stopping at the first that shows something. The one under it is only looked for when a blend
     * needs it. */
    p = obj_visible ? obj->prio[X] : -1;
    n = line.count - 1;
#define NEXT_LAYER(LAYER_, COLOUR_)     for (;;) {         int bg;         uint16_t index;         if (p >= 0 && (n < 0 || line.order_prio[n] >= p)) {             p = -1;             LAYER_ = LAYER_OBJ;             COLOUR_ = (obj->colour[X] & 0x8000) ? (uint16_t)(obj->colour[X] & 0x7FFF) : PAL[obj->colour[X] - 1];             break;         }         if (n < 0)             break;         bg = line.order[n--];         index = line_bg[bg][X];         if (index && (mask & (1 << bg))) {             LAYER_ = bg;             COLOUR_ = PAL[index - 1];             break;         }     }
    NEXT_LAYER(top, top_colour)
    semi = top == LAYER_OBJ ? obj->flags[X] & 1 : 0;
    if (semi || ((mask & 0x20) && (bldcnt & (1 << top)) && ((bldcnt >> 6) & 3) == 1))
        NEXT_LAYER(second, second_colour)
#undef NEXT_LAYER
    if (semi && (bldcnt & (0x100 << second))) {
        top_colour = blend(top_colour, second_colour, line.eva, line.evb);
    } else if ((mask & 0x20) && (bldcnt & (1 << top))) {
        switch ((bldcnt >> 6) & 3) {
        case 1:
            if (bldcnt & (0x100 << second)) top_colour = blend(top_colour, second_colour, line.eva, line.evb);
            break;
        case 2: top_colour = fade(top_colour, line.evy, 1); break;
        case 3: top_colour = fade(top_colour, line.evy, 0); break;
        }
    }
    if (line.edge_fade && (top == LAYER_BACKDROP || (top == LAYER_OBJ ? obj->prio[X] > 0 : view.bg[top].map != NULL))) {
        /* the last columns of a map narrower than the view sink into the black beside it */
        int from_left = X - view.hud_left, from_right = view.hud_right - 1 - X, by = -1;
        if (view.hud_left > 0 && from_left >= 0 && from_left < EDGE_FADE) by = from_left;
        if (view.hud_right < view.width && from_right >= 0 && from_right < EDGE_FADE) by = from_right;
        if (by >= 0) {
            /* a soft curve: most of the darkening in the last few columns, a hint of it further in */
            int rest = EDGE_FADE - by;                  /* 1 at the inner end .. EDGE_FADE at the edge */
            top_colour = fade(top_colour, rest * rest * 16 / (EDGE_FADE * EDGE_FADE), 0);
        }
    }
    *moved = top == LAYER_OBJ ? obj->flags[X] & 4 : top < 4 ? line.moved[top] : 0;
    return top_colour;
}

void ss_ppu_render(uint32_t *pixels, int pitch_pixels, const struct ss_view *requested)
{
    static uint32_t place[H][4];        /* where each background was on each line of the last frame */
    int mode = 0, y, X, n;
    uint8_t end_of_frame[SS_LINE_REGISTERS];
    int per_line = ss_line_registers(0) != NULL;

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
    find_alternating();
    line.edge_fade = view.world && (view.hud_left > 0 || view.hud_right < view.width);
    page_shown = view.world && view.width > W && (IO16(0) & 7) == 0 && page_open();
    hud_at_edges = view.world && view.width > W && (IO16(0) & 7) == 0 && !page_shown;
    for (y = 0; y < H; y++) {
        uint32_t *out = pixels + y * pitch_pixels;
        uint8_t *calm = still + y * view.width;
        int two;
        if (per_line)
            memcpy((void *)(uintptr_t)SS_IO, ss_line_registers(y), SS_LINE_REGISTERS);
        if (y == 0 || per_line) {
            line.dispcnt = IO16(0); line.bldcnt = IO16(0x50);
            mode = line.dispcnt & 7; line.eva = IO16(0x52) & 31; line.evb = (IO16(0x52) >> 8) & 31; line.evy = IO16(0x54) & 31;
            line.windows = line.dispcnt & 0xE000;
            if (line.eva > 16) line.eva = 16;
            if (line.evb > 16) line.evb = 16;
            if (line.evy > 16) line.evy = 16;
            for (n = 0; n < 4; n++) {
                line.prio[n] = IO16(0x08 + n * 2) & 3;
                line.enabled[n] = (line.dispcnt & (0x100 << n)) && ((mode == 0) || (mode == 1 && n < 3) || (mode == 2 && n >= 2));
            }
            line.count = 0;
            for (X = 3; X >= 0; X--)
                for (n = 3; n >= 0; n--)
                    if (line.enabled[n] && line.prio[n] == X) {
                        line.order[line.count] = n;
                        line.order_prio[line.count++] = X;
                    }
        }
        for (n = 0; n < 4; n++) {
            /* where the background is: its scroll, or everything that places a rotated one */
            uint32_t at = IO32(0x10 + n * 4) & 0x01FF01FFu;
            if (n >= 2 && mode != 0)
                at = IO32(0x20 + (n - 2) * 0x10) * 31u + IO32(0x24 + (n - 2) * 0x10) * 131u + IO32(0x28 + (n - 2) * 0x10) * 7u + IO32(0x2C + (n - 2) * 0x10);
            if (view.bg[n].map)
                at += (uint32_t)view.shift << 9;
            line.moved[n] = at != place[y][n];
            if (frame_is_new)
                place[y][n] = at;
        }
        if (line.dispcnt & 0x80) {
            for (X = 0; X < view.width; X++) out[X] = 0xFFFFFFFFu;
            memset(calm, 1, (size_t)view.width);
            continue;
        }
        for (n = 0; n < 4; n++) {
            if (!line.enabled[n]) continue;
            if ((mode == 1 && n == 2) || mode == 2) affine_background(n, y);
            else text_background(n, y);
        }
        two = two_pictures[y] && (line.dispcnt & 0x1000);
        if (line.dispcnt & 0x1000) sprites(y, two);
        for (X = 0; X < view.width; X++) {
            int moved, moved_other;
            uint32_t colour;
            if (!view.world && view.width > W && native_x(X, y) < 0 && y >= view.hud_rows) {
                out[X] = 0x00000000u;     /* transparent: the platform shows its own field there */       /* side bars of a scene that keeps the console's composition */
                calm[X] = 0;
                continue;
            }
            colour = to_rgb(compose(X, y, &obj_now, &moved));
            if (two && (obj_now.colour[X] != obj_other.colour[X] || obj_now.prio[X] != obj_other.prio[X] || obj_now.flags[X] != obj_other.flags[X])) {
                /* something alternates here: half of this frame's picture, half of the other frame's */
                uint32_t other = to_rgb(compose(X, y, &obj_other, &moved_other));
                colour = 0xFF000000u | (((colour >> 1) & 0x7F7F7F) + ((other >> 1) & 0x7F7F7F) + ((colour | other) & 0x010101));
                moved |= moved_other;
            }
            out[X] = colour;
            calm[X] = (uint8_t)!moved;
        }
    }
    if (per_line)
        memcpy((void *)(uintptr_t)SS_IO, end_of_frame, sizeof end_of_frame);
    if (frame_is_new) {
        n = (int)(frame_drawn % KEPT);
        memcpy(kept[n].oam, OAM, sizeof kept[n].oam);
        memcpy(kept[n].tiles, VRAM + 0x10000, sizeof kept[n].tiles);
        memcpy(kept[n].colours, PAL + 0x100, sizeof kept[n].colours);
    }
}
