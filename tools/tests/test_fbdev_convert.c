/* The framebuffer's pixel routines (runtime/include/shiningsource/fbdev.h) on any machine: the 4:3 canvas of
 * 320x240 game pixels at 2x into a 640x480 screen, in each format the GA36 may have (XRGB8888 on goodluckOS,
 * RGB565 on older firmwares, and their BGR forms), with the decor written once and then only the picture's
 * rectangle. The expected values are computed here independently of the routines.
 *
 *   clang -O2 -I runtime/include tools/tests/test_fbdev_convert.c -o test_fbdev_convert && ./test_fbdev_convert
 *
 * Built for ARM with NEON (-mfpu=neon) it checks the NEON paths too. Prints "fbdev convert: ok" and exits 0. */
#include "shiningsource/fbdev.h"

#include <stdio.h>
#include <stdlib.h>

enum { CW = 320, CH = 240, SW = 640, SH = 480, RX = 40, RY = 40, RW = 240, RH = 160, PAD = 24 };

static uint32_t decor[CW * CH], other_decor[CW * CH], frame[CW * CH];
static int failures;

static uint32_t blend(uint32_t argb, uint32_t under)
{
    unsigned a = argb >> 24;
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned top = argb >> shift & 255, low = under >> shift & 255;
        out |= (uint32_t)((top * a + low * (255 - a) + 127) / 255) << shift;
    }
    return out;
}

static uint32_t expected(int format, uint32_t c)
{
    unsigned r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    switch (format) {
    case SS_FB_RGB565: return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case SS_FB_BGR565: return (b >> 3) << 11 | (g >> 2) << 5 | r >> 3;
    case SS_FB_XRGB8888: return 0xFF000000u | r << 16 | g << 8 | b;
    default: return 0xFF000000u | b << 16 | g << 8 | r;
    }
}

static uint32_t screen_at(const uint8_t *screen, size_t line, int bytes, int x, int y)
{
    const uint8_t *at = screen + (size_t)y * line + (size_t)x * (size_t)bytes;
    if (bytes == 2) {
        uint16_t v;
        memcpy(&v, at, 2);
        return v;
    }
    uint32_t v;
    memcpy(&v, at, 4);
    return v;
}

/* Every screen pixel of the canvas against what it should show. `with_decor`: the decor around the rectangle is
 * `now`; otherwise it must still be `before` (it was not written). */
static void check(const char *what, const uint8_t *screen, size_t line, int format, int scale, const uint32_t *now,
                  const uint32_t *before, int left, int top)
{
    int bytes = ss_fb_format_bytes(format), told = 0;
    for (int y = 0; y < CH * scale; y++)
        for (int x = 0; x < CW * scale; x++) {
            int cx = x / scale, cy = y / scale, inside = cx >= RX && cx < RX + RW && cy >= RY && cy < RY + RH;
            uint32_t want = inside ? blend(frame[(cy - RY) * RW + (cx - RX)], now[cy * CW + cx]) : before[cy * CW + cx];
            uint32_t got = screen_at(screen, line, bytes, left + x, top + y);
            want = expected(format, want);
            if (got != want) {
                if (told++ < 4)
                    fprintf(stderr, "%s: format %d scale %d: screen (%d, %d) is %08X, wanted %08X\n", what, format, scale, x, y,
                            (unsigned)got, (unsigned)want);
                failures++;
            }
        }
}

static void check_untouched(const char *what, const uint8_t *screen, size_t line, int bytes, int left, int top, int w, int h)
{
    int told = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < (int)(line / (size_t)bytes); x++) {
            if (x >= left && x < left + w && y >= top && y < top + h)
                continue;
            if (screen[(size_t)y * line + (size_t)x * (size_t)bytes] != 0xA5) {
                if (told++ < 4)
                    fprintf(stderr, "%s: wrote outside the canvas at (%d, %d)\n", what, x, y);
                failures++;
            }
        }
}

int main(void)
{
    static const int formats[4] = { SS_FB_XRGB8888, SS_FB_RGB565, SS_FB_XBGR8888, SS_FB_BGR565 };
    uint32_t seed = 12345;
    for (int i = 0; i < CW * CH; i++) {
        seed = seed * 1103515245u + 12345u;
        decor[i] = 0xFF000000u | (seed >> 8 & 0xFFFFFF);
        seed = seed * 1103515245u + 12345u;
        other_decor[i] = 0xFF000000u | (seed >> 8 & 0xFFFFFF);
    }
    for (int i = 0; i < RW * RH; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t alpha = (i % 7 == 0) ? 0 : (i % 11 == 0) ? (seed >> 24 & 255) : 255;   /* transparent, half, opaque */
        frame[i] = alpha << 24 | (seed >> 4 & 0xFFFFFF);
    }
    for (int f = 0; f < 4; f++)
        for (int scale = 1; scale <= 2; scale++) {
            const int format = formats[f], bytes = ss_fb_format_bytes(format);
            const size_t line = (size_t)(SW + PAD) * (size_t)bytes;     /* a line longer than the screen, as fbdev may have */
            const int left = (SW - CW * scale) / 2, top = (SH - CH * scale) / 2;
            uint8_t *screen = malloc(line * SH), *base;
            char what[64];
            if (!screen)
                return 2;
            memset(screen, 0xA5, line * SH);
            base = screen + (size_t)top * line + (size_t)left * (size_t)bytes;
            /* the first frame: decor and rectangle */
            ss_fb_canvas_blit(base, line, format, scale, 1, decor, CW, CH, frame, RW, RX, RY, RW, RH);
            snprintf(what, sizeof what, "full");
            check(what, screen, line, format, scale, decor, decor, left, top);
            check_untouched(what, screen, line, bytes, left, top, CW * scale, CH * scale);
            /* the next frames: only the rectangle; the decor on the screen stays as it was, the rectangle's
             * transparent pixels show the (unchanged) decor under them */
            for (int i = 0; i < RW * RH; i += 3)
                frame[i] ^= 0x00102030u;
            ss_fb_canvas_blit(base, line, format, scale, 0, decor, CW, CH, frame, RW, RX, RY, RW, RH);
            snprintf(what, sizeof what, "rectangle only");
            check(what, screen, line, format, scale, decor, decor, left, top);
            /* a new decor: written around the rectangle again */
            ss_fb_canvas_blit(base, line, format, scale, 1, other_decor, CW, CH, frame, RW, RX, RY, RW, RH);
            snprintf(what, sizeof what, "new decor");
            check(what, screen, line, format, scale, other_decor, other_decor, left, top);
            check_untouched(what, screen, line, bytes, left, top, CW * scale, CH * scale);
            free(screen);
        }
    /* odd widths and offsets for the rows' tails (NEON handles 4 or 8 pixels at a time) */
    for (int f = 0; f < 4; f++)
        for (int w = 1; w <= 19; w++) {
            const int format = formats[f], bytes = ss_fb_format_bytes(format);
            uint8_t out[2 * 4 * 32];
            memset(out, 0, sizeof out);
            ss_fb_row(out, format, frame + 5, decor + 3, w, 2, 0);
            for (int x = 0; x < 2 * w; x++) {
                uint32_t want = expected(format, blend(frame[5 + x / 2], decor[3 + x / 2])), got;
                if (bytes == 2) { uint16_t v; memcpy(&v, out + 2 * x, 2); got = v; }
                else memcpy(&got, out + 4 * x, 4);
                if (got != want) {
                    if (failures < 20)
                        fprintf(stderr, "row: format %d width %d pixel %d is %08X, wanted %08X\n", format, w, x, (unsigned)got, (unsigned)want);
                    failures++;
                }
            }
        }
#ifdef SS_FB_NEON
    printf("fbdev convert (NEON): %s\n", failures ? "FAILED" : "ok");
#else
    printf("fbdev convert: %s\n", failures ? "FAILED" : "ok");
#endif
    return failures != 0;
}
