#ifndef SHININGSOURCE_WIDE_ART_H
#define SHININGSOURCE_WIDE_ART_H

#include <stdint.h>

/* Apply scene artwork to the columns outside the original 240-pixel view. */
int ss_wide_art_fill(uint32_t *pixels, int width, int scene, int allow_ultrawide);

#endif
