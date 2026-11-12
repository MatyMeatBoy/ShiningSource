/* The game image's side of the platform layer: everything goes to the 64-bit host through the bridge. */
#include "bridge.h"
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>

const uint32_t *ss_bridge;
extern char __image_start[], __image_end[], __text_end[];

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

uint16_t ss_plat_present(const uint32_t *pixels)
{
    return (uint16_t)SS_HOST(SS_B_PRESENT, uint32_t (*)(uint32_t))((uint32_t)(uintptr_t)pixels);
}

void ss_plat_audio(const int16_t *samples, int frames)
{
    SS_HOST(SS_B_AUDIO, void (*)(uint32_t, int))((uint32_t)(uintptr_t)samples, frames);
}

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
    AgbMain();
    exit(0);
}
