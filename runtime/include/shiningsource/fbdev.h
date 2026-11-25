#ifndef SHININGSOURCE_FBDEV_H
#define SHININGSOURCE_FBDEV_H

/* Direct Linux framebuffer output and raw evdev input for handhelds without a usable GPU (GA36-MB: Allwinner A33,
 * 640x480 RGB565 /dev/fb0). The functions are in runtime/plat/fbdev_linux.c, which is empty on other systems; the
 * pixel routines below are portable so that tools/tests/test_fbdev_convert.c can check them anywhere.
 *
 * Environment: SS_FBDEV (the device, or a regular file for tests), SS_FBDEV_MODE=WxHxBPP (a file's geometry,
 * 640x480x16 when not given), SS_FB_VIEW (the picture's width, 320 by default), SS_FB_FLIP=0 (no page flipping),
 * SS_FB_VSYNC=1 (wait for the display's refresh before a frame), SS_FB_TTY (the console to put in graphics mode,
 * "none" for none), SS_FB_SHOT=N (write the screen at presented frame N to shiningsource_fb.raw), SS_EVDEV,
 * SS_EVDEV_MAP and SS_INPUT_LOG (see ss_evdev_start). */

#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SS_FB_NEON 1
#endif

enum ss_fb_format { SS_FB_RGB565, SS_FB_BGR565, SS_FB_XRGB8888, SS_FB_XBGR8888 };

/* An ARGB pixel over an opaque one, as the renderer blends the picture over what is under it. */
static inline uint32_t ss_fb_over(uint32_t argb, uint32_t under)
{
    unsigned a = argb >> 24, rest = 255 - a;
    if (a == 255)
        return argb;
    if (a == 0)
        return under | 0xFF000000u;
    return 0xFF000000u
        | ((((argb >> 16) & 255) * a + ((under >> 16) & 255) * rest + 127) / 255) << 16
        | ((((argb >> 8) & 255) * a + ((under >> 8) & 255) * rest + 127) / 255) << 8
        | (((argb & 255) * a + (under & 255) * rest + 127) / 255);
}

static inline uint16_t ss_fb_565(uint32_t c)
{
    return (uint16_t)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F));
}

static inline uint32_t ss_fb_swap_rb(uint32_t c)
{
    return (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
}

/* One row of `w` source pixels, each repeated `scale` times, into `dst` in the framebuffer's format. `opaque`: the
 * alpha byte means nothing (XRGB); otherwise pixels not fully opaque are blended over `under` (NULL: black). */
static inline void ss_fb_row(void *dst, int format, const uint32_t *src, const uint32_t *under, int w, int scale, int opaque)
{
    int x = 0, k;
#ifdef SS_FB_NEON
    if (format == SS_FB_RGB565 && (scale == 1 || scale == 2)) {
        uint16_t *out = (uint16_t *)dst;
        for (; x + 8 <= w; x += 8) {
            uint8x8x4_t p = vld4_u8((const uint8_t *)(src + x));       /* B, G, R, A planes */
            uint16x8_t c;
            if (!opaque && vget_lane_u64(vreinterpret_u64_u8(vmvn_u8(p.val[3])), 0)) {
                for (k = 0; k < 8; k++) {
                    uint16_t v = ss_fb_565(ss_fb_over(src[x + k], under ? under[x + k] : 0));
                    if (scale == 1) out[x + k] = v;
                    else out[2 * (x + k)] = out[2 * (x + k) + 1] = v;
                }
                continue;
            }
            c = vsriq_n_u16(vshll_n_u8(p.val[2], 8), vshll_n_u8(p.val[1], 8), 5);
            c = vsriq_n_u16(c, vshll_n_u8(p.val[0], 8), 11);
            if (scale == 1) {
                vst1q_u16(out + x, c);
            } else {
                uint16x8x2_t d = vzipq_u16(c, c);
                vst1q_u16(out + 2 * x, d.val[0]);
                vst1q_u16(out + 2 * x + 8, d.val[1]);
            }
        }
    } else if (format == SS_FB_XRGB8888 && (scale == 1 || scale == 2)) {
        uint32_t *out = (uint32_t *)dst;
        const uint32x4_t alpha = vdupq_n_u32(0xFF000000u);
        for (; x + 4 <= w; x += 4) {
            uint32x4_t c = vld1q_u32(src + x);
            if (!opaque) {
                uint32x4_t full = vceqq_u32(vandq_u32(c, alpha), alpha);
                uint32x2_t both = vand_u32(vget_low_u32(full), vget_high_u32(full));
                if (!(vget_lane_u32(both, 0) & vget_lane_u32(both, 1))) {
                    for (k = 0; k < 4; k++) {
                        uint32_t v = ss_fb_over(src[x + k], under ? under[x + k] : 0);
                        if (scale == 1) out[x + k] = v;
                        else out[2 * (x + k)] = out[2 * (x + k) + 1] = v;
                    }
                    continue;
                }
            }
            c = vorrq_u32(c, alpha);
            if (scale == 1) {
                vst1q_u32(out + x, c);
            } else {
                uint32x4x2_t d = vzipq_u32(c, c);
                vst1q_u32(out + 2 * x, d.val[0]);
                vst1q_u32(out + 2 * x + 4, d.val[1]);
            }
        }
    }
#endif
    if (format == SS_FB_RGB565 || format == SS_FB_BGR565) {
        uint16_t *out = (uint16_t *)dst + x * scale;
        for (; x < w; x++) {
            uint32_t c = opaque ? src[x] : ss_fb_over(src[x], under ? under[x] : 0);
            uint16_t v = ss_fb_565(format == SS_FB_BGR565 ? ss_fb_swap_rb(c) : c);
            for (k = 0; k < scale; k++)
                *out++ = v;
        }
    } else {
        uint32_t *out = (uint32_t *)dst + x * scale;
        for (; x < w; x++) {
            uint32_t c = (opaque ? src[x] : ss_fb_over(src[x], under ? under[x] : 0)) | 0xFF000000u;
            if (format == SS_FB_XBGR8888)
                c = ss_fb_swap_rb(c);
            for (k = 0; k < scale; k++)
                *out++ = c;
        }
    }
}

/* ---- runtime/plat/fbdev_linux.c ---- */
int ss_fb_open(void);                   /* SS_FBDEV: 1 when the framebuffer is in use, after the log line */
int ss_fb_active(void);
int ss_fb_width(void);
int ss_fb_height(void);
int ss_fb_view(void);                   /* SS_FB_VIEW, 320 by default, within 240..480 and the panel's width */
/* The game's `width` x 160 ARGB picture (rows `stride` apart) at the largest whole scale, centred; transparent
 * pixels show `under` (width x 160, NULL: black). Returns 0 when it does not fit (the caller composes instead). */
int ss_fb_present_picture(const uint32_t *pixels, int width, int stride, const uint32_t *under);
/* A composed XRGB8888 frame (normally the panel's size), centred. */
void ss_fb_present_full(const void *pixels, int pitch, int w, int h);
void ss_fb_close(void);                 /* registered with atexit by ss_fb_open; logs the frame counts */

enum { SS_EVDEV_MENU = 1 << 10 };       /* the GBA buttons are bits 0..9, as KEYINPUT */
int ss_evdev_start(void);               /* opens /dev/input/event*: the number of devices kept */
unsigned ss_evdev_poll(void);           /* the buttons held now (and SS_EVDEV_MENU) */

#endif
