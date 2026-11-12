/* The table of services the 64-bit host gives the game image.
 *
 * The image is 64-bit code with 32-bit pointers; the host is an ordinary 64-bit program linked below 4 GiB, so
 * each entry is the 32-bit address of a host function. Every function uses the System V calling convention
 * on every operating system, and takes pointers as 32-bit integers. Add entries at the end only.
 */
#ifndef SS_BRIDGE_H
#define SS_BRIDGE_H
#include <stdint.h>

enum {
    SS_B_VERSION,           /* uint32_t (void)                                   table version, checked at start */
    SS_B_WRITE,             /* int (int handle, u32 data, u32 size)              1 = stdout, 2 = stderr, else a file */
    SS_B_OPEN,              /* int (u32 path, u32 mode)                          0 on failure */
    SS_B_READ,              /* int (int handle, u32 data, u32 size) */
    SS_B_CLOSE,             /* int (int handle) */
    SS_B_REMOVE,            /* int (u32 path) */
    SS_B_RENAME,            /* int (u32 from, u32 to) */
    SS_B_GETENV,            /* int (u32 name, u32 out, u32 capacity)             length, or -1 if not set */
    SS_B_EXIT,              /* void (int code) */
    SS_B_VIDEO_INIT,        /* int (u32 title) */
    SS_B_PRESENT,           /* u32 (u32 pixels, int width)                       returns the held buttons */
    SS_B_AUDIO,             /* void (u32 samples, int frames) */
    SS_B_STACK_ALLOC,       /* u32 (u32 size)                                    low memory for a coroutine stack */
    SS_B_STACK_FREE,        /* void (u32 base, u32 size) */
    SS_B_STACK_BOUNDS,      /* void (u32 low, u32 high)                          tell the system which stack runs now */
    SS_B_POW, SS_B_SIN, SS_B_COS, SS_B_FMOD,    /* double (double[, double]) */
    SS_B_VIEW_WIDTH,        /* int (void)                                        picture width that fills the window */
    SS_B_PUMP,              /* void (void) */
    SS_B_NET_OPEN,          /* int (u32 host, int port) */
    SS_B_NET_SEND,          /* int (u32 data, int size) */
    SS_B_NET_RECV,          /* int (u32 data, int capacity) */
    SS_B_NET_WAIT,          /* void (int milliseconds) */
    SS_B_NET_FAREWELL,      /* void (u32 data, int size) */
    SS_B_TICKS,             /* u32 (void)                                        milliseconds */
    SS_B_STATUS,            /* void (u32 text) */
    SS_B_COUNT
};
#define SS_BRIDGE_VERSION 4u

extern const uint32_t *ss_bridge;
#define SS_HOST(index, type) ((type)(uintptr_t)ss_bridge[index])

#endif
