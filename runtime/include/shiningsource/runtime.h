/* ShiningSource runtime: the platform layer under the game. Nothing here knows which game it serves. */
#ifndef SHININGSOURCE_RUNTIME_H
#define SHININGSOURCE_RUNTIME_H

#include <stdint.h>

/* GBA address space, mapped at its hardware addresses (dev32 and host64 modes). */
#define SS_EWRAM   0x02000000u   /* 256 KiB */
#define SS_IWRAM   0x03000000u   /*  32 KiB */
#define SS_IO      0x04000000u   /*   1 KiB of registers */
#define SS_PALETTE 0x05000000u   /*   1 KiB */
#define SS_VRAM    0x06000000u   /*  96 KiB */
#define SS_OAM     0x07000000u   /*   1 KiB */
#define SS_ROM     0x08000000u   /* the user's cartridge dump */

/* Map every region and load the ROM file. Returns 0 on success; on failure `error` says which step failed. */
int ss_memory_init(const char *rom_path, const char **error);

#define SS_SCREEN_W 240
#define SS_SCREEN_H 160

#define SS_MAX_VIEW_W 480

/* How a picture wider than the console's 240 pixels is composed (see core/ppu.c). */
struct ss_bg_source {
    const uint16_t *map;        /* full tile map of this background (screen entries), or NULL */
    int width, height;          /* in tiles */
    int scroll_x, scroll_y;     /* world pixel shown at native (0, 0) */
};
struct ss_view {
    int width;                  /* 240..SS_MAX_VIEW_W */
    int world;                  /* the scene has source maps: sprites move with the view */
    int shift;                  /* view column where native x = 0 lands */
    int hud_rows;               /* top lines whose halves go to the edges of the visible picture */
    int hud_left, hud_right;    /* those edges, as view columns */
    struct ss_bg_source bg[4];
};

/* Draw the current frame into 32-bit ARGB pixels, `view->width` wide (240 when view is NULL). */
void ss_ppu_render(uint32_t *pixels, int pitch_pixels, const struct ss_view *view);
/* Shining Soul engine: describe the field scene's maps for a picture `width` wide. 0 if not a field scene. */
int ss_field_view(struct ss_view *view, int width);

/* ---- services every platform provides to the core ---- */

/* Window and sound device. */
int ss_plat_video_init(const char *title);
/* Width the picture should have to fill the window at a whole-number scale (240..SS_MAX_VIEW_W). */
int ss_plat_view_width(void);
/* Show one `width` x 160 ARGB frame, wait for the next 1/59.7275 s, return the held buttons (KEYINPUT bit order). */
uint16_t ss_plat_present(const uint32_t *pixels, int width);
/* Play one game frame of 48 kHz stereo samples. */
void ss_plat_audio(const int16_t *samples, int frames);
/* Coroutines for the game's task scheduler. ss_co_switch(NULL) returns to the thread that called ss_co_init. */
void ss_co_init(void);
void *ss_co_create(void (*entry)(void *), void *argument);
void ss_co_switch(void *coroutine);
void ss_co_delete(void *coroutine);
/* Is this address inside the program's own code? */
int ss_is_native_code(uint32_t address);

/* Sound output: one game frame of 48 kHz stereo samples. ss_audio_report prints what was produced. */
void ss_audio_push(const int16_t *samples, int frames);
void ss_audio_report(void);

/* Replace ROM words that hold game function addresses by the native functions. Returns how many. */
unsigned ss_patch_code_pointers(void);

/* Absorb a memory access the console would have ignored (see stray.c). `context` is the CPU context of the fault. */
int ss_stray_access(void *context, uint32_t address, int is_write);
int ss_divide_by_zero(void *context);
unsigned long ss_stray_count(void);

/* Frames completed so far. */
unsigned long ss_frame_number(void);

/* Called by a generated stub the first time a function nobody implements yet is reached. */
void ss_stub(const char *name, int *seen);

/* Number of distinct stubs reached so far (the port's "what is missing right now" counter). */
int ss_stub_count(void);

#endif
