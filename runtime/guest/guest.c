/* The game image's side of the platform layer: everything goes to the 64-bit host through the bridge. */
#include "bridge.h"
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>

const uint32_t *ss_bridge;
#ifdef __MACH__         /* arm64_32 image: the linker names the bounds of the code section */
extern char __image_start[] __asm("section$start$__TEXT$__text");
extern char __text_end[] __asm("section$end$__TEXT$__text");
#define __image_end __text_end
#else
extern char __image_start[], __image_end[], __text_end[];
#endif

void AgbMain(void);

/* ---- coroutines: a saved stack pointer per coroutine, stacks in low memory from the host ---- */

enum { STACK_BYTES = 480 * 1024 };
struct Coroutine { uint64_t sp; uint32_t stack_low, stack_high; };
void ss_co_swap(uint64_t *from, uint64_t *to);
void ss_co_start(void);

static struct Coroutine main_coroutine, *running = &main_coroutine;
static struct Coroutine pool[256];

void ss_co_init(void)
{
    /* the host entered the image on a stack it described with SS_B_STACK_BOUNDS; nothing to record here */
}

void *ss_co_create(void (*entry)(void *), void *argument)
{
    struct Coroutine *co = NULL;
    uint64_t *frame;
    int i;

    for (i = 0; i < 256; i++) {
        if (!pool[i].stack_low) { co = &pool[i]; break; }
    }
    if (!co) {
        fprintf(stderr, "coroutines: none left\n");
        exit(1);
    }
    co->stack_low = SS_HOST(SS_B_STACK_ALLOC, uint32_t (*)(uint32_t))(STACK_BYTES);
    co->stack_high = co->stack_low + STACK_BYTES;
#ifdef __aarch64__
    /* what ss_co_swap loads: x19..x30 and d8..d15 (22 slots); x30 is where it returns */
    frame = (uint64_t *)(uintptr_t)(co->stack_high - 192);
    for (i = 0; i < 24; i++)
        frame[i] = 0;
    frame[0] = (uint64_t)(uintptr_t)argument;   /* x19 */
    frame[1] = (uint64_t)(uintptr_t)entry;      /* x20 */
    frame[11] = (uint64_t)(uintptr_t)ss_co_start;       /* x30 */
#else
    /* what ss_co_swap pops: r15 r14 r13 r12 rbx rbp, then the return address */
    frame = (uint64_t *)(uintptr_t)(co->stack_high - 64);
    frame[0] = 0;                               /* r15 */
    frame[1] = 0;                               /* r14 */
    frame[2] = (uint64_t)(uintptr_t)argument;   /* r13 */
    frame[3] = (uint64_t)(uintptr_t)entry;      /* r12 */
    frame[4] = 0;                               /* rbx */
    frame[5] = 0;                               /* rbp */
    frame[6] = (uint64_t)(uintptr_t)ss_co_start;
    frame[7] = 0;
#endif
    co->sp = (uint64_t)(uintptr_t)frame;
    return co;
}

void ss_co_switch(void *coroutine)
{
    struct Coroutine *from = running, *to = coroutine ? coroutine : &main_coroutine;
    if (from == to)
        return;
    running = to;
    SS_HOST(SS_B_STACK_BOUNDS, void (*)(uint32_t, uint32_t))(to->stack_low, to->stack_high);
    ss_co_swap(&from->sp, &to->sp);
}

void ss_co_delete(void *coroutine)
{
    struct Coroutine *co = coroutine;
    if (co == running)
        return;                                 /* a coroutine cannot free the stack it runs on */
    SS_HOST(SS_B_STACK_FREE, void (*)(uint32_t, uint32_t))(co->stack_low, STACK_BYTES);
    co->stack_low = 0;
}

int ss_is_native_code(uint32_t address)
{
    return address >= (uint32_t)(uintptr_t)__image_start && address < (uint32_t)(uintptr_t)__text_end;
}

/* ---- video, input, sound ---- */

int ss_plat_video_init(const char *title)
{
    return SS_HOST(SS_B_VIDEO_INIT, int (*)(uint32_t))((uint32_t)(uintptr_t)title);
}

int ss_plat_view_width(void)
{
    return SS_HOST(SS_B_VIEW_WIDTH, int (*)(void))();
}

uint16_t ss_plat_present(const uint32_t *pixels, int width)
{
    return (uint16_t)SS_HOST(SS_B_PRESENT, uint32_t (*)(uint32_t, int))((uint32_t)(uintptr_t)pixels, width);
}

void ss_plat_audio(const int16_t *samples, int frames)
{
    SS_HOST(SS_B_AUDIO, void (*)(uint32_t, int))((uint32_t)(uintptr_t)samples, frames);
}

void ss_plat_status(const char *text) { SS_HOST(SS_B_STATUS, void (*)(uint32_t))((uint32_t)(uintptr_t)text); }
void ss_plat_overlay(int darkness, const char *text) { SS_HOST(SS_B_OVERLAY, void (*)(int, uint32_t))(darkness, (uint32_t)(uintptr_t)text); }
void ss_plat_pump(void) { SS_HOST(SS_B_PUMP, void (*)(void))(); }
int ss_plat_net_open(const char *host, int port) { return SS_HOST(SS_B_NET_OPEN, int (*)(uint32_t, int))((uint32_t)(uintptr_t)host, port); }
int ss_plat_net_send(const void *data, int size) { return SS_HOST(SS_B_NET_SEND, int (*)(uint32_t, int))((uint32_t)(uintptr_t)data, size); }
int ss_plat_net_recv(void *data, int capacity) { return SS_HOST(SS_B_NET_RECV, int (*)(uint32_t, int))((uint32_t)(uintptr_t)data, capacity); }
void ss_plat_net_wait(int milliseconds) { SS_HOST(SS_B_NET_WAIT, void (*)(int))(milliseconds); }
void ss_plat_net_farewell(const void *data, int size) { SS_HOST(SS_B_NET_FAREWELL, void (*)(uint32_t, int))((uint32_t)(uintptr_t)data, size); }
uint32_t ss_plat_ticks_ms(void) { return SS_HOST(SS_B_TICKS, uint32_t (*)(void))(); }
/* the host evaluates the achievements: it sees the same memory (a host without them serves an empty function) */
static void host_achievements(int run_flags) { SS_HOST(SS_B_ACHIEVEMENTS, void (*)(int))(run_flags); }

/* ---- entry: called by the host on a low stack, with the GBA memory mapped and the ROM loaded ---- */

void ss_guest_entry(const uint32_t *bridge, uint32_t stack_low, uint32_t stack_high)
{
    ss_bridge = bridge;
    if (SS_HOST(SS_B_VERSION, uint32_t (*)(void))() != SS_BRIDGE_VERSION)
        exit(90);
    main_coroutine.stack_low = stack_low;
    main_coroutine.stack_high = stack_high;
    fprintf(stderr, "%u code pointers in the ROM data now point to native functions\n", ss_patch_code_pointers());
    fprintf(stderr, "game image %p-%p entered; running the game's AgbMain\n", (void *)__image_start, (void *)__image_end);
    ss_rebel_set_host(host_achievements);
    ss_rebel_start();                   /* before the game reads its save: a Rebel run has its own file */
    AgbMain();
    exit(0);
}
