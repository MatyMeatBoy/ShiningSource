/* Direct framebuffer output and raw evdev input for Linux handhelds without a usable GPU (GA36-MB V1.2: Allwinner
 * A33, 640x480 RGB565 /dev/fb0 on the stock sunxi 3.4 kernel). SDL runs with its offscreen video driver; the
 * platform layer (runtime/win32/plat_sdl.c, built with SS_FBDEV=1) hands each frame here: the game's picture alone
 * at a whole scale (ss_fb_present_picture) or the frame SDL composed with what goes over it (ss_fb_present_full).
 * See shiningsource/fbdev.h for the environment. Every platform compiles this file; it is empty except on Linux. */
#if defined(__linux__) && !defined(__ANDROID__)

#include "shiningsource/fbdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

enum { PICTURE_H = 160, VIEW_MIN = 240, VIEW_MAX = 480, VIEW_DEFAULT = 320 };

static int fb_fd = -1, fb_is_file, fb_format, fb_bytes, fb_pages = 1, fb_shown, fb_vsync, fb_view = VIEW_DEFAULT;
static uint8_t *fb_map, *fb_shadow;     /* the mapping, or a copy written with pwrite when it cannot be mapped */
static size_t fb_map_size;
static struct fb_var_screeninfo fb_var;
static struct fb_fix_screeninfo fb_fix;
static uint32_t fb_first_yoffset;
static unsigned fb_dirty;               /* pages whose borders may hold something: cleared before a picture alone */
static int fb_last_width, fb_last_scale;
static unsigned long fb_fast, fb_generic, fb_presented, fb_shot_at, fb_canvas_full;
static int tty_fd = -1, tty_mode = KD_TEXT;
/* The 4:3 canvas. What each page last received: the decor's id and the rectangle written over it; a page whose
 * decor is current gets only the rectangle. */
static struct { unsigned id; int valid, canvas_w, canvas_h, scale, x, y, w, h; } fb_canvas[2];

int ss_fb_active(void) { return fb_map || fb_shadow; }
int ss_fb_width(void) { return (int)fb_var.xres; }
int ss_fb_height(void) { return (int)fb_var.yres; }
int ss_fb_view(void) { return fb_view; }

/* ---- the console: graphics mode and no cursor while the game runs, text again at the end ---- */
static void console_restore(void)
{
    static const char show[] = "\033[?25h";
    if (tty_fd < 0)
        return;
    ioctl(tty_fd, KDSETMODE, tty_mode);
    if (write(tty_fd, show, sizeof show - 1) < 0) { /* nothing to do */ }
    close(tty_fd);
    tty_fd = -1;
}

static void console_graphics(void)
{
    static const char hide[] = "\033[?25l";
    const char *named = getenv("SS_FB_TTY");
    const char *candidates[3] = { named, "/dev/tty0", "/dev/tty" };
    int i, mode;
    if (named && (!*named || !strcmp(named, "none") || !strcmp(named, "0")))
        return;
    for (i = named ? 0 : 1; i < 3 && tty_fd < 0; i++) {
        int fd = candidates[i] ? open(candidates[i], O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
        if (fd < 0)
            continue;
        if (ioctl(fd, KDGETMODE, &mode) == 0) {
            tty_fd = fd;
            tty_mode = mode;
            if (write(fd, hide, sizeof hide - 1) < 0) { /* the cursor may stay */ }
            if (ioctl(fd, KDSETMODE, KD_GRAPHICS) != 0)
                fprintf(stderr, "fbdev: %s stays in text mode: %s\n", candidates[i], strerror(errno));
            return;
        }
        close(fd);
    }
    fprintf(stderr, "fbdev: no console to put in graphics mode\n");
}

static void fatal_signal(int sig)
{
    console_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- the framebuffer ---- */
static uint8_t *page_base(int page)
{
    if (fb_shadow)
        return fb_shadow;
    return fb_map + (size_t)(fb_pages > 1 ? (uint32_t)page * fb_var.yres : fb_var.yoffset) * fb_fix.line_length
        + (size_t)fb_var.xoffset * (size_t)fb_bytes;
}

static void clear_rect(uint8_t *base, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++)
        memset(base + (size_t)(y + row) * fb_fix.line_length + (size_t)x * (size_t)fb_bytes, 0, (size_t)w * (size_t)fb_bytes);
}

static int parse_mode(const char *text, unsigned *w, unsigned *h, unsigned *bpp)
{
    unsigned a, b, c = 16;
    int n = text ? sscanf(text, "%ux%ux%u", &a, &b, &c) : 0;
    if (n < 2 || !a || !b || a > 8192 || b > 8192 || (c != 16 && c != 32))
        return 0;
    *w = a; *h = b; *bpp = c;
    return 1;
}

int ss_fb_open(void)
{
    const char *path = getenv("SS_FBDEV"), *text;
    struct stat st;
    int fd;
    uint64_t row_end, map_size;
    if (ss_fb_active())
        return 1;
    if (!path || !*path)
        return 0;
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "fbdev: cannot open %s: %s\n", path, strerror(errno));
        if (fd >= 0) close(fd);
        return 0;
    }
    memset(&fb_var, 0, sizeof fb_var);
    memset(&fb_fix, 0, sizeof fb_fix);
    fb_is_file = S_ISREG(st.st_mode);
    if (fb_is_file) {
        /* a file stands for the panel in tests: 640x480 RGB565 unless SS_FBDEV_MODE says otherwise */
        unsigned w = 640, h = 480, bpp = 16;
        if (getenv("SS_FBDEV_MODE") && !parse_mode(getenv("SS_FBDEV_MODE"), &w, &h, &bpp))
            fprintf(stderr, "fbdev: SS_FBDEV_MODE is WxHxBPP (16 or 32): 640x480x16 used\n");
        fb_var.xres = fb_var.xres_virtual = w;
        fb_var.yres = fb_var.yres_virtual = h;
        fb_var.bits_per_pixel = bpp;
        fb_var.red.offset = bpp == 16 ? 11 : 16;
        fb_var.green.offset = bpp == 16 ? 5 : 8;
        fb_fix.line_length = w * bpp / 8;
        fb_fix.smem_len = fb_fix.line_length * h;
        if ((uint64_t)st.st_size < fb_fix.smem_len && ftruncate(fd, fb_fix.smem_len) != 0) {
            fprintf(stderr, "fbdev: cannot size %s: %s\n", path, strerror(errno));
            close(fd);
            return 0;
        }
    } else if (ioctl(fd, FBIOGET_VSCREENINFO, &fb_var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fb_fix) != 0) {
        fprintf(stderr, "fbdev: %s is not a framebuffer: %s\n", path, strerror(errno));
        close(fd);
        return 0;
    }
    if (fb_var.bits_per_pixel != 16 && fb_var.bits_per_pixel != 32) {
        fprintf(stderr, "fbdev: %s has %u bits per pixel; only 16 and 32 are drawn\n", path, fb_var.bits_per_pixel);
        close(fd);
        return 0;
    }
    fb_bytes = (int)fb_var.bits_per_pixel / 8;
    fb_format = fb_bytes == 2 ? (fb_var.red.offset == 0 ? SS_FB_BGR565 : SS_FB_RGB565)
                              : (fb_var.red.offset == 0 ? SS_FB_XBGR8888 : SS_FB_XRGB8888);
    row_end = ((uint64_t)fb_var.xoffset + fb_var.xres) * (uint64_t)fb_bytes;
    map_size = (uint64_t)fb_fix.line_length * fb_var.yres_virtual;
    if (fb_fix.smem_len && map_size > fb_fix.smem_len)
        map_size = fb_fix.smem_len;
    if (!fb_var.xres || !fb_var.yres || fb_var.xres > 8192 || fb_var.yres > 8192 || row_end > fb_fix.line_length
        || (uint64_t)fb_var.yoffset + fb_var.yres > fb_var.yres_virtual
        || ((uint64_t)fb_var.yoffset + fb_var.yres - 1) * fb_fix.line_length + row_end > map_size) {
        fprintf(stderr, "fbdev: %s has an unusable geometry (%ux%u, virtual %ux%u, line %u)\n", path, fb_var.xres, fb_var.yres,
                fb_var.xres_virtual, fb_var.yres_virtual, fb_fix.line_length);
        close(fd);
        return 0;
    }
    fb_first_yoffset = fb_var.yoffset;
    text = getenv("SS_FB_FLIP");
    fb_pages = !fb_is_file && !(text && text[0] == '0') && fb_var.yres_virtual >= 2 * fb_var.yres && fb_var.xoffset == 0
        && (uint64_t)fb_fix.line_length * 2 * fb_var.yres <= map_size ? 2 : 1;
    fb_shown = fb_pages > 1 && fb_var.yoffset >= fb_var.yres;
    fb_map_size = (size_t)map_size;
    fb_map = mmap(NULL, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_map == MAP_FAILED) {
        fb_map = NULL;
        fb_pages = 1;
        fb_shadow = calloc(fb_var.yres, fb_fix.line_length);
        if (!fb_shadow) {
            close(fd);
            return 0;
        }
        fprintf(stderr, "fbdev: %s cannot be mapped (%s): written instead\n", path, strerror(errno));
    }
    fb_fd = fd;
    text = getenv("SS_FB_VIEW");
    fb_view = text && atoi(text) > 0 ? atoi(text) : VIEW_DEFAULT;
    if (fb_view < VIEW_MIN) fb_view = VIEW_MIN;
    if (fb_view > VIEW_MAX) fb_view = VIEW_MAX;
    if (fb_view > (int)fb_var.xres && (int)fb_var.xres >= VIEW_MIN) fb_view = (int)fb_var.xres;
    fb_view &= ~1;
    fb_vsync = getenv("SS_FB_VSYNC") && getenv("SS_FB_VSYNC")[0] == '1';
    fb_shot_at = getenv("SS_FB_SHOT") ? strtoul(getenv("SS_FB_SHOT"), NULL, 10) : 0;
    {
        int scale_x = (int)fb_var.xres / fb_view, scale_y = (int)fb_var.yres / PICTURE_H;
        fprintf(stderr, "fbdev: %s %ux%u, %u bpp, line %u, scale %d, view %d%s%s\n", path, fb_var.xres, fb_var.yres, fb_var.bits_per_pixel,
                fb_fix.line_length, scale_x < scale_y ? scale_x : scale_y, fb_view, fb_pages > 1 ? ", 2 pages" : "",
                fb_is_file ? ", file" : "");
    }
    /* the whole screen once: from then on a picture alone writes only itself and the borders stay black */
    for (int page = 0; page < fb_pages; page++)
        clear_rect(page_base(page), 0, 0, (int)fb_var.xres, (int)fb_var.yres);
    if (fb_shadow && pwrite(fb_fd, fb_shadow, (size_t)fb_fix.line_length * fb_var.yres,
                            (off_t)fb_var.yoffset * fb_fix.line_length) < 0)
        fprintf(stderr, "fbdev: cannot write %s: %s\n", path, strerror(errno));
    fb_dirty = 0;
    if (!fb_is_file) {
        static const int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
        console_graphics();
        for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; i++)
            signal(signals[i], fatal_signal);
    }
    atexit(ss_fb_close);
    return 1;
}

static void write_shot(int page)
{
    FILE *file = fopen("shiningsource_fb.raw", "wb");
    const uint8_t *base = page_base(page);
    size_t row = (size_t)fb_var.xres * (size_t)fb_bytes;
    for (unsigned y = 0; file && y < fb_var.yres; y++)
        fwrite(base + (size_t)y * fb_fix.line_length, 1, row, file);
    if (file)
        fclose(file);
    fprintf(stderr, "fbdev: frame %lu %s shiningsource_fb.raw (%ux%u, %u bpp, rows packed)\n", fb_presented,
            file ? "written to" : "could not be written to", fb_var.xres, fb_var.yres, fb_var.bits_per_pixel);
}

/* The page to draw in: the hidden one when flipping. A wait for the refresh first, if asked and possible. */
static int frame_begin(void)
{
    if (fb_vsync && fb_pages == 1) {
        __u32 screen = 0;
        if (ioctl(fb_fd, FBIO_WAITFORVSYNC, &screen) != 0) {
            fprintf(stderr, "fbdev: no wait for the refresh (%s): frames are paced by the clock alone\n", strerror(errno));
            fb_vsync = 0;
        }
    }
    return fb_pages > 1 ? !fb_shown : 0;
}

static void frame_end(int page)
{
    if (fb_shadow && pwrite(fb_fd, fb_shadow, (size_t)fb_fix.line_length * fb_var.yres, (off_t)fb_var.yoffset * fb_fix.line_length) < 0) {
        static int told;
        if (!told++)
            fprintf(stderr, "fbdev: write failed: %s\n", strerror(errno));
    }
    if (fb_pages > 1) {
        struct fb_var_screeninfo pan = fb_var;
        pan.yoffset = (uint32_t)page * fb_var.yres;
        if (ioctl(fb_fd, FBIOPAN_DISPLAY, &pan) == 0) {
            fb_shown = page;
        } else {
            /* no flipping here: draw where the screen is from now on (this frame is not seen) */
            fprintf(stderr, "fbdev: page flipping refused (%s): one page\n", strerror(errno));
            fb_pages = 1;
            fb_var.yoffset = (uint32_t)fb_shown * fb_var.yres;
            fb_dirty = ~0u;
        }
    }
    fb_presented++;
    if (fb_shot_at && fb_presented == fb_shot_at)
        write_shot(fb_pages > 1 ? fb_shown : 0);
}

int ss_fb_present_picture(const uint32_t *pixels, int width, int stride, const uint32_t *under)
{
    int scale, out_w, out_h, left, top, page;
    uint8_t *base;
    size_t row_bytes;
    if (!ss_fb_active() || width <= 0 || !pixels)
        return 0;
    scale = (int)fb_var.xres / width < (int)fb_var.yres / PICTURE_H ? (int)fb_var.xres / width : (int)fb_var.yres / PICTURE_H;
    if (scale < 1)
        return 0;
    out_w = width * scale;
    out_h = PICTURE_H * scale;
    left = ((int)fb_var.xres - out_w) / 2;
    top = ((int)fb_var.yres - out_h) / 2;
    if (width != fb_last_width || scale != fb_last_scale) {
        fb_dirty = ~0u;                 /* a narrower picture leaves the old one's edges: black again */
        fb_last_width = width;
        fb_last_scale = scale;
    }
    page = frame_begin();
    base = page_base(page);
    if (fb_dirty & (1u << page)) {
        clear_rect(base, 0, 0, (int)fb_var.xres, top);
        clear_rect(base, 0, top + out_h, (int)fb_var.xres, (int)fb_var.yres - top - out_h);
        clear_rect(base, 0, top, left, out_h);
        clear_rect(base, left + out_w, top, (int)fb_var.xres - left - out_w, out_h);
        fb_dirty &= ~(1u << page);
    }
    row_bytes = (size_t)out_w * (size_t)fb_bytes;
    for (int y = 0; y < PICTURE_H; y++) {
        uint8_t *row = base + (size_t)(top + y * scale) * fb_fix.line_length + (size_t)left * (size_t)fb_bytes;
        ss_fb_row(row, fb_format, pixels + (size_t)y * (size_t)stride, under ? under + (size_t)y * (size_t)width : NULL, width, scale, 0);
        for (int k = 1; k < scale; k++)
            memcpy(row + (size_t)k * fb_fix.line_length, row, row_bytes);
    }
    fb_canvas[page].valid = 0;
    fb_fast++;
    frame_end(page);
    return 1;
}

int ss_fb_present_canvas(const uint32_t *decor, int canvas_w, int canvas_h, unsigned decor_id,
                         const uint32_t *pixels, int stride, int x, int y, int w, int h)
{
    int scale, left, top, page, full;
    if (!ss_fb_active() || !decor || canvas_w <= 0 || canvas_h <= 0)
        return 0;
    scale = (int)fb_var.xres / canvas_w < (int)fb_var.yres / canvas_h ? (int)fb_var.xres / canvas_w : (int)fb_var.yres / canvas_h;
    if (scale < 1)
        return 0;
    if (x < 0) { w += x; pixels -= x; x = 0; }
    if (y < 0) { h += y; pixels -= (ptrdiff_t)y * stride; y = 0; }
    if (x + w > canvas_w) w = canvas_w - x;
    if (y + h > canvas_h) h = canvas_h - y;
    if (w <= 0 || h <= 0 || !pixels)
        w = h = 0;
    left = ((int)fb_var.xres - canvas_w * scale) / 2;
    top = ((int)fb_var.yres - canvas_h * scale) / 2;
    page = frame_begin();
    full = (fb_dirty >> page & 1) || !fb_canvas[page].valid || fb_canvas[page].id != decor_id
        || fb_canvas[page].canvas_w != canvas_w || fb_canvas[page].canvas_h != canvas_h || fb_canvas[page].scale != scale
        || fb_canvas[page].x != x || fb_canvas[page].y != y || fb_canvas[page].w != w || fb_canvas[page].h != h;
    if (full && (left > 0 || top > 0) && (fb_dirty >> page & 1)) {
        uint8_t *base = page_base(page);
        clear_rect(base, 0, 0, (int)fb_var.xres, top);
        clear_rect(base, 0, top + canvas_h * scale, (int)fb_var.xres, (int)fb_var.yres - top - canvas_h * scale);
        clear_rect(base, 0, top, left, canvas_h * scale);
        clear_rect(base, left + canvas_w * scale, top, (int)fb_var.xres - left - canvas_w * scale, canvas_h * scale);
    }
    ss_fb_canvas_blit(page_base(page) + (size_t)top * fb_fix.line_length + (size_t)left * (size_t)fb_bytes, fb_fix.line_length,
                      fb_format, scale, full, decor, canvas_w, canvas_h, pixels, stride, x, y, w, h);
    fb_dirty &= ~(1u << page);
    fb_canvas[page].id = decor_id;
    fb_canvas[page].valid = 1;
    fb_canvas[page].canvas_w = canvas_w;
    fb_canvas[page].canvas_h = canvas_h;
    fb_canvas[page].scale = scale;
    fb_canvas[page].x = x;
    fb_canvas[page].y = y;
    fb_canvas[page].w = w;
    fb_canvas[page].h = h;
    fb_last_width = 0;                  /* a picture alone after this clears the screen first */
    if (full)
        fb_canvas_full++;
    fb_fast++;
    frame_end(page);
    return 1;
}

void ss_fb_present_full(const void *pixels, int pitch, int w, int h)
{
    int page, left, top;
    uint8_t *base;
    if (!ss_fb_active() || !pixels || w <= 0 || h <= 0)
        return;
    page = frame_begin();
    base = page_base(page);
    if (w > (int)fb_var.xres) w = (int)fb_var.xres;
    if (h > (int)fb_var.yres) h = (int)fb_var.yres;
    left = ((int)fb_var.xres - w) / 2;
    top = ((int)fb_var.yres - h) / 2;
    if (left || top)
        clear_rect(base, 0, 0, (int)fb_var.xres, (int)fb_var.yres);
    for (int y = 0; y < h; y++)
        ss_fb_row(base + (size_t)(top + y) * fb_fix.line_length + (size_t)left * (size_t)fb_bytes, fb_format,
                  (const uint32_t *)((const uint8_t *)pixels + (size_t)y * (size_t)pitch), NULL, w, 1, 1);
    fb_dirty |= 1u << page;             /* what was composed around the picture stays until cleared */
    fb_generic++;
    frame_end(page);
}

void ss_fb_close(void)
{
    if (!ss_fb_active())
        return;
    fprintf(stderr, "fbdev: %lu frames presented: %lu by the direct path (%lu with the canvas decor redrawn), %lu composed\n",
            fb_presented, fb_fast, fb_canvas_full, fb_generic);
    if (fb_pages > 1 && fb_var.yoffset != fb_first_yoffset) {
        struct fb_var_screeninfo pan = fb_var;
        pan.yoffset = fb_first_yoffset;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &pan);
    }
    console_restore();
    if (fb_map)
        munmap(fb_map, fb_map_size);
    free(fb_shadow);
    fb_map = fb_shadow = NULL;
    close(fb_fd);
    fb_fd = -1;
}

/* ---- raw evdev input ----
 * For when SDL finds neither a controller nor a keyboard (the offscreen video driver reads no keyboard, and a
 * handheld's built-in buttons may be a gpio-keys device SDL does not take for a joystick). Every
 * /dev/input/event* with keys or a pad is read, without blocking. Keys go through a table, which SS_EVDEV_MAP
 * changes: "code=button,..." with the Linux code in decimal or 0x hex and the button one of a b select start
 * right left up down r l menu none. ABS_X/ABS_Y and ABS_HAT0X/ABS_HAT0Y are the directions. SS_INPUT_LOG=1 writes
 * every device and event to stderr. */
enum { EVDEV_MAX = 16, MAP_MAX = 96, BIT_NONE = -1, BIT_MENU = 10 };
static const char *const button_names[] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "menu" };
static struct { uint16_t code; int8_t bit; } key_map[MAP_MAX] = {
    { KEY_UP, 6 }, { KEY_DOWN, 7 }, { KEY_LEFT, 5 }, { KEY_RIGHT, 4 },
    { BTN_DPAD_UP, 6 }, { BTN_DPAD_DOWN, 7 }, { BTN_DPAD_LEFT, 5 }, { BTN_DPAD_RIGHT, 4 },
    { BTN_EAST, 0 }, { BTN_SOUTH, 1 }, { BTN_TL, 9 }, { BTN_TR, 8 }, { BTN_TL2, 9 }, { BTN_TR2, 8 },
    { BTN_SELECT, 2 }, { BTN_START, 3 }, { BTN_MODE, BIT_MENU },
    { KEY_X, 0 }, { KEY_Z, 1 }, { KEY_C, 9 }, { KEY_V, 8 }, { KEY_ENTER, 3 }, { KEY_RIGHTSHIFT, 2 }, { KEY_ESC, BIT_MENU },
    { KEY_LEFTCTRL, 0 }, { KEY_LEFTALT, 1 }, { KEY_SPACE, 2 }, { KEY_PAGEUP, 9 }, { KEY_PAGEDOWN, 8 },
};
static int key_map_count = 29;
static struct evdev {
    int fd, number;
    int axis_min[4], axis_max[4], axis[4];          /* ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y */
    int logged[ABS_CNT];
    unsigned keys;                                  /* the mapped buttons this device holds */
    uint8_t held[KEY_CNT / 8 + 1];
} devices[EVDEV_MAX];
static int device_count, evdev_log;

static int map_bit(unsigned code)
{
    for (int i = 0; i < key_map_count; i++)
        if (key_map[i].code == code)
            return key_map[i].bit;
    return BIT_NONE;
}

static void read_map(const char *text)
{
    while (text && *text) {
        char *end, name[16];
        unsigned long code = strtoul(text, &end, 0);
        size_t length;
        int bit = -2, i;
        if (end == text || *end != '=')
            break;
        text = end + 1;
        length = strcspn(text, ",");
        if (length < sizeof name) {
            memcpy(name, text, length);
            name[length] = 0;
            if (!strcmp(name, "none")) bit = BIT_NONE;
            for (i = 0; i < (int)(sizeof button_names / sizeof button_names[0]); i++)
                if (!strcmp(name, button_names[i])) bit = i;
        }
        if (bit == -2 || code >= KEY_CNT) {
            fprintf(stderr, "evdev: SS_EVDEV_MAP entry %lu=%.*s not understood\n", code, (int)length, text);
        } else {
            for (i = 0; i < key_map_count && key_map[i].code != code; i++) {}
            if (i == key_map_count && key_map_count < MAP_MAX)
                key_map_count++;
            if (i < key_map_count) {
                key_map[i].code = (uint16_t)code;
                key_map[i].bit = (int8_t)bit;
            }
        }
        text += length;
        if (*text == ',')
            text++;
    }
}

#define BIT_SET(array, n) ((array)[(n) / (8 * sizeof *(array))] >> ((n) % (8 * sizeof *(array))) & 1)

int ss_evdev_start(void)
{
    static int started;
    const char *text = getenv("SS_EVDEV");
    if (started)
        return device_count;
    started = 1;
    if (text && text[0] == '0')
        return 0;
    evdev_log = getenv("SS_INPUT_LOG") && getenv("SS_INPUT_LOG")[0] == '1';
    read_map(getenv("SS_EVDEV_MAP"));
    for (int number = 0; number < 64 && device_count < EVDEV_MAX; number++) {
        char path[32], name[128] = "?";
        unsigned long types[EV_MAX / (8 * sizeof(long)) + 1] = { 0 }, keys[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 },
                      axes[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
        struct input_id id = { 0 };
        struct evdev *device = &devices[device_count];
        int fd, mapped = 0, pad = 0;
        snprintf(path, sizeof path, "/dev/input/event%d", number);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        ioctl(fd, EVIOCGID, &id);
        ioctl(fd, EVIOCGBIT(0, sizeof types), types);
        if (BIT_SET(types, EV_KEY))
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        if (BIT_SET(types, EV_ABS))
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof axes), axes);
        for (int i = 0; i < key_map_count; i++)
            mapped += key_map[i].bit != BIT_NONE && BIT_SET(keys, key_map[i].code);
        memset(device, 0, sizeof *device);
        for (int a = 0; a < 4; a++) {
            static const int codes[4] = { ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y };
            struct input_absinfo info;
            if (BIT_SET(axes, codes[a]) && ioctl(fd, EVIOCGABS(codes[a]), &info) == 0 && info.maximum > info.minimum) {
                device->axis_min[a] = info.minimum;
                device->axis_max[a] = info.maximum;
                device->axis[a] = info.value;
                pad = 1;
            }
        }
        if (evdev_log || mapped || pad)
            fprintf(stderr, "evdev: %s \"%s\" bus %04x vendor %04x product %04x version %04x: %d mapped keys%s%s\n", path, name,
                    id.bustype, id.vendor, id.product, id.version, mapped, pad ? ", directions" : "",
                    mapped || pad ? "" : ", not used");
        if (!mapped && !pad) {
            close(fd);
            continue;
        }
        device->fd = fd;
        device->number = number;
        for (int a = 0; a < ABS_CNT; a++)
            device->logged[a] = 1 << 30;
        device_count++;
    }
    fprintf(stderr, "evdev: reading %d device%s directly\n", device_count, device_count == 1 ? "" : "s");
    return device_count;
}

static unsigned axis_bits(const struct evdev *device)
{
    unsigned bits = 0;
    for (int a = 0; a < 4; a++) {
        int range = device->axis_max[a] - device->axis_min[a], middle = device->axis_min[a] + range / 2;
        if (range <= 0)
            continue;
        if (device->axis[a] > middle + range / 4)
            bits |= a & 1 ? 1u << 7 : 1u << 4;             /* down, right */
        if (device->axis[a] < middle - range / 4)
            bits |= a & 1 ? 1u << 6 : 1u << 5;             /* up, left */
    }
    return bits;
}

unsigned ss_evdev_poll(void)
{
    unsigned bits = 0;
    for (int d = 0; d < device_count; d++) {
        struct evdev *device = &devices[d];
        struct input_event events[32];
        ssize_t got;
        if (device->fd < 0)
            continue;
        while ((got = read(device->fd, events, sizeof events)) > 0) {
            for (int i = 0; i < (int)(got / (ssize_t)sizeof events[0]); i++) {
                const struct input_event *e = &events[i];
                if (e->type == EV_KEY && e->code < KEY_CNT && e->value != 2) {
                    int bit = map_bit(e->code);
                    if (e->value)
                        device->held[e->code / 8] |= (uint8_t)(1 << (e->code % 8));
                    else
                        device->held[e->code / 8] &= (uint8_t)~(1 << (e->code % 8));
                    if (evdev_log)
                        fprintf(stderr, "evdev: event%d key %u (0x%x) %s -> %s\n", device->number, e->code, e->code,
                                e->value ? "down" : "up", bit >= 0 ? button_names[bit] : "nothing");
                } else if (e->type == EV_ABS && e->code < ABS_CNT) {
                    int a = e->code == ABS_X ? 0 : e->code == ABS_Y ? 1 : e->code == ABS_HAT0X ? 2 : e->code == ABS_HAT0Y ? 3 : -1;
                    unsigned before = axis_bits(device);
                    if (a >= 0)
                        device->axis[a] = e->value;
                    if (evdev_log) {
                        int last = device->logged[e->code], step = a >= 0 && device->axis_max[a] - device->axis_min[a] > 8
                            ? (device->axis_max[a] - device->axis_min[a]) / 8 : 1;
                        if (axis_bits(device) != before || last == 1 << 30 || e->value - last >= step || last - e->value >= step) {
                            device->logged[e->code] = e->value;
                            fprintf(stderr, "evdev: event%d axis %u value %d%s\n", device->number, e->code, e->value,
                                    a >= 0 ? " (direction)" : "");
                        }
                    }
                }
            }
        }
        if (got < 0 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "evdev: event%d is gone: %s\n", device->number, strerror(errno));
            close(device->fd);
            device->fd = -1;
            continue;
        }
        device->keys = 0;
        for (int i = 0; i < key_map_count; i++)
            if (key_map[i].bit != BIT_NONE && (device->held[key_map[i].code / 8] >> (key_map[i].code % 8) & 1))
                device->keys |= 1u << key_map[i].bit;
        bits |= device->keys | axis_bits(device);
    }
    return bits;
}

#else
typedef int ss_fbdev_linux_unused;      /* ISO C wants something in a translation unit */
#endif
