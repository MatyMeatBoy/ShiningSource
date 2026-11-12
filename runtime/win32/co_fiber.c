/* Coroutines for the dev32 build: Windows fibers. */
#include "shiningsource/runtime.h"

#include <windows.h>

extern char ss_gba_arena[];
static void *main_fiber;

void ss_co_init(void)
{
    main_fiber = ConvertThreadToFiber(NULL);
}

void *ss_co_create(void (*entry)(void *), void *argument)
{
    return CreateFiber(512 * 1024, (LPFIBER_START_ROUTINE)entry, argument);
}

void ss_co_switch(void *coroutine)
{
    SwitchToFiber(coroutine ? coroutine : main_fiber);
}

void ss_co_delete(void *coroutine)
{
    DeleteFiber(coroutine);
}

/* Is this address inside the program's own code (as opposed to a GBA address that was never translated)? */
int ss_is_native_code(uint32_t address)
{
    return address >= 0x01800000u && address < (uint32_t)(uintptr_t)ss_gba_arena;
}
