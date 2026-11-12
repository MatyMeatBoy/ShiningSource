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

/* Draw the current frame from the video registers and memory into 32-bit ARGB pixels. */
void ss_ppu_render(uint32_t *pixels, int pitch_pixels);

/* Window and input. ss_video_frame presents a frame, reads the keyboard and waits for the next 1/59.7275 s. */
int ss_video_init(const char *title);
void ss_video_frame(void);

/* Replace ROM words that hold game function addresses by the native functions. Returns how many. */
unsigned ss_patch_code_pointers(void);

/* Called by a generated stub the first time a function nobody implements yet is reached. */
void ss_stub(const char *name, int *seen);

/* Number of distinct stubs reached so far (the port's "what is missing right now" counter). */
int ss_stub_count(void);

#endif
