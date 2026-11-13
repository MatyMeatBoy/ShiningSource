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
/* Exit code with which the game asks to be started again (its SoftReset). */
enum { SS_EXIT_RESTART = 75 };

void ss_ppu_render(uint32_t *pixels, int pitch_pixels, const struct ss_view *view);
/* The video registers as they were on scanline y of the frame being shown, or NULL when the game changed
 * nothing during the frame (then the registers themselves are the whole frame's). */
enum { SS_LINE_REGISTERS = 0x58 };
const uint8_t *ss_line_registers(int y);
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
/* Skip scene (core/scene.c). ss_skip_update is called once a frame with the buttons the player holds and returns
 * the buttons the game is given; `view` says how to show the frame. */
struct ss_skip_view {
    int darkness;               /* 0..255, black over the picture (the fade) */
    const char *text;           /* one line to show over the picture, or NULL */
    int present;                /* 0: do not show this frame (the game is running ahead) */
    int mute;                   /* 1: drop this frame's sound */
};
int ss_scene_active(void);
uint16_t ss_skip_update(uint16_t held, struct ss_skip_view *view);
/* Online side of it (core/netlink.c): a vote per player, and one frame of the shared timeline to start at. */
int ss_net_active(void);
void ss_net_vote(int wants);
void ss_net_votes(int *have, int *need);
int ss_net_skip_now(void);
unsigned long ss_net_label(void);
/* The game's one-time messages, online (core/netlink.c, "messages"): whole, in order, once to every player.
 * Called from the game's own sender and receivers through games/<game>/port/patches.json. */
int ss_net_msg_online(void);
int ss_net_msg_send(const void *data, int size);
int ss_net_msg_recv(int who, void *data, int capacity);
int ss_net_msg_gone(int who);
void ss_net_flush(void);       /* end of a game frame: send this frame's block now */
/* What the next presented frame carries over the picture: black of the given strength and one line of text
 * (capital letters, digits and ? = / : . only). */
void ss_plat_overlay(int darkness, const char *text);

/* A notice over the top of the picture for a few seconds: a heading and one line (an achievement's title, UTF-8,
 * shown as capitals) and, if given, a square icon of `icon_size` x `icon_size` ARGB pixels (at most 128; it is
 * copied). `rebel` gives it the Rebel record's colours. Up to four wait their turn. */
void ss_plat_notice(const char *heading, const char *text, int rebel, const uint32_t *icon, int icon_size);

/* Rebel mode (core/rebel.c). A run started with cheats (SS_CHEATS) is a Rebel run until the program ends: it
 * keeps its own save file, and its achievements go to a local record and never to RetroAchievements.
 * ss_rebel_start is called before the game's main (it chooses the save file); ss_rebel_frame once per frame: it
 * applies the cheats, never while an online session is active, and hands the frame to the achievements of the
 * platform, if it registered any (runtime/achievements). */
enum { SS_RUN_REBEL = 1, SS_RUN_CHEATING = 2, SS_RUN_ONLINE = 4 };
void ss_rebel_start(void);
void ss_rebel_frame(void);
void ss_rebel_set_host(void (*frame)(int run_flags));

/* Handle window events without showing a frame (used while a frame waits for the network). */
void ss_plat_pump(void);
/* One line about what the program is doing, shown in the window title (NULL or "" clears it). */
void ss_plat_status(const char *text);
/* Network: one UDP socket to the relay. open returns 0 on success; recv returns 0 when nothing is waiting;
 * wait sleeps until a packet arrives or the time passes; farewell is a packet sent when the program exits. */
int ss_plat_net_open(const char *host, int port);
int ss_plat_net_send(const void *data, int size);
int ss_plat_net_recv(void *data, int capacity);
void ss_plat_net_wait(int milliseconds);
void ss_plat_net_farewell(const void *data, int size);
uint32_t ss_plat_ticks_ms(void);
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
