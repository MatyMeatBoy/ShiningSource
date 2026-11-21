#ifndef SHININGSOURCE_SOUL_FIELD_H
#define SHININGSOURCE_SOUL_FIELD_H

#include <stddef.h>
#include <stdint.h>

/* The Soul frame's blue-green field and its lattice of points of light. */
static inline void ss_soul_field_pixels(uint32_t *pixels, int w, int h, int left, int top, int right, int bottom)
{
    for (int y = 0; y < h; y++) {
        int t = y * 255 / (h > 1 ? h - 1 : 1);
        uint32_t colour = 0xFF000000u | (uint32_t)(20 - t * 10 / 255) << 16 |
                         (uint32_t)(62 - t * 34 / 255) << 8 | (uint32_t)(74 - t * 36 / 255);
        for (int x = 0; x < w; x++)
            pixels[(size_t)y * w + x] = colour;
    }
    for (int y = 3; y < h; y += 7)
        for (int x = 3 + (y * 5) % 11; x < w; x += 13) {
            if (x >= left && x < right && y >= top && y < bottom)
                continue;
            int k = (x * 31 + y * 17) % 5;
            pixels[(size_t)y * w + x] = 0xFF000000u | (uint32_t)(60 + k * 22) << 16 |
                                       (uint32_t)(120 + k * 20) << 8 | (uint32_t)(130 + k * 20);
        }
}

#endif
