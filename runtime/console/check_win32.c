/* The consoles' memory scheme on a PC, to prove it before a console runs it (tools/build_rebased.py).
 *
 * A 32-bit Windows program like the development build, but with the game's memory in the block of
 * runtime/console/rebase.c instead of at the GBA's addresses. The GBA's own address range is part of the image
 * here too, only to keep the system from using it, and every page of it is made inaccessible: an access that
 * still used a hardware address stops the program. No fault is absorbed; any fault is reported as a crash.
 */
#include "rebase.h"
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

void AgbMain(void);

char ss_reserved[0x08000000];       /* spans 0x02000000-0x09000000, as the development build's arena does */

static LONG CALLBACK report_crash(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    static int reporting;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION
        && record->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && record->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (reporting)
        ExitProcess(4);
    reporting = 1;
    fprintf(stderr, "crash: exception %08lX at eip %08lX", record->ExceptionCode, info->ContextRecord->Eip);
    if (record->NumberParameters >= 2)
        fprintf(stderr, ", %s address %08lX", record->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long)record->ExceptionInformation[1]);
    fprintf(stderr, " at frame %lu\n", ss_frame_number());
    fflush(stderr);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- coroutines: Windows fibers ---- */

static void *main_fiber;

void ss_co_init(void) { main_fiber = ConvertThreadToFiber(NULL); }
void *ss_co_create(void (*entry)(void *), void *argument) { return CreateFiber(512 * 1024, (LPFIBER_START_ROUTINE)entry, argument); }
void ss_co_switch(void *coroutine) { SwitchToFiber(coroutine ? coroutine : main_fiber); }
void ss_co_delete(void *coroutine) { DeleteFiber(coroutine); }

int main(int argc, char **argv)
{
    const char *error = "";
    uintptr_t low = (uintptr_t)ss_reserved, high = low + sizeof ss_reserved;
    DWORD old;

    AddVectoredExceptionHandler(1, report_crash);
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.gba\n", argv[0]);
        return 2;
    }
    if (low > SS_EWRAM || high < SS_ROM + 0x1000000) {
        fprintf(stderr, "the reserved range is %08lX-%08lX: it must cover the GBA's addresses\n", (unsigned long)low, (unsigned long)high);
        return 1;
    }
    low = (low + 0xFFF) & ~(uintptr_t)0xFFF;
    high &= ~(uintptr_t)0xFFF;
    if (!VirtualProtect((void *)low, high - low, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "the GBA's address range could not be closed\n");
        return 1;
    }
    if (ss_memory_init(argv[1], &error) != 0) {
        fprintf(stderr, "memory setup failed: %s\n", error);
        return 1;
    }
    fprintf(stderr, "GBA addresses %08lX-%08lX closed; the game's memory is at %p\n", (unsigned long)low, (unsigned long)high, (void *)ss_arena);
    fprintf(stderr, "%u code pointers in the ROM data now point to native functions\n", ss_patch_code_pointers());
    AgbMain();
    return 0;
}
