/* Entry point of the development build: map the GBA memory, load the ROM, run the game's own main. */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <windows.h>

void AgbMain(void);

/* A crash must say where. Addresses inside the executable are printed for tools/where.py to name. */
static LONG CALLBACK report_crash(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const uint32_t *stack = (const uint32_t *)(uintptr_t)info->ContextRecord->Esp;
    int i, shown = 0;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION
        && record->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && record->ExceptionCode != EXCEPTION_STACK_OVERFLOW
        && record->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "crash: exception %08lX at eip %08lX", record->ExceptionCode, info->ContextRecord->Eip);
    if (record->NumberParameters >= 2)
        fprintf(stderr, ", %s address %08lX", record->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long)record->ExceptionInformation[1]);
    fprintf(stderr, "\nstack words that point into the program:");
    for (i = 0; i < 400 && shown < 12; i++) {
        if (IsBadReadPtr(stack + i, 4))
            break;
        if (stack[i] >= 0x10000000u && stack[i] < 0x11000000u) {
            fprintf(stderr, " %08lX", (unsigned long)stack[i]);
            shown++;
        }
    }
    fprintf(stderr, "\n%d distinct stub(s) had been reached\n", ss_stub_count());
    fflush(stderr);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    const char *error = "";

    AddVectoredExceptionHandler(1, report_crash);
    if (argc < 2) {
        fprintf(stderr, "usage: %s ROM.gba\n", argv[0]);
        return 2;
    }
    if (ss_memory_init(argv[1], &error) != 0) {
        fprintf(stderr, "memory setup failed: %s\n", error);
        return 1;
    }
    fprintf(stderr, "%u code pointers in the ROM data now point to native functions\n", ss_patch_code_pointers());
    fprintf(stderr, "memory mapped, ROM loaded; entering the game's AgbMain\n");
    AgbMain();
    fprintf(stderr, "AgbMain returned; %d distinct stub(s) reached\n", ss_stub_count());
    return 0;
}
