/* Entry point of the development build: map the GBA memory, load the ROM, run the game's own main. */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>

void AgbMain(void);

static unsigned long watch_from;

/* A crash must say where. Addresses inside the executable are printed for tools/where.py to name. */
static LONG CALLBACK report_crash(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const uint32_t *stack = (const uint32_t *)(uintptr_t)info->ContextRecord->Esp;
    int i, shown = 0;

    /* SS_WATCH: a write to the watched address; say who wrote it, from the frame given in SS_WATCH_FROM on */
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && (info->ContextRecord->Dr6 & 1)) {
        if (ss_frame_number() >= watch_from) {
            fprintf(stderr, "watch: frame %lu, written by code before eip %08lX; callers:", ss_frame_number(), info->ContextRecord->Eip);
            for (i = 0; i < 200 && shown < 6; i++) {
                if (stack[i] >= 0x01800000u && stack[i] < 0x02000000u) {
                    fprintf(stderr, " %08lX", (unsigned long)stack[i]);
                    shown++;
                }
            }
            fprintf(stderr, "\n");
        }
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION
        && record->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && record->ExceptionCode != EXCEPTION_STACK_OVERFLOW
        && record->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    /* an access the console would have ignored, made by game code (not by the runtime or the system) */
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2
        && info->ContextRecord->Eip >= 0x01800000u && info->ContextRecord->Eip < 0x02000000u
        && ss_stray_access(info->ContextRecord, (uint32_t)record->ExceptionInformation[1], record->ExceptionInformation[0] != 0))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (record->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO && info->ContextRecord->Eip >= 0x01800000u
        && info->ContextRecord->Eip < 0x02000000u && ss_divide_by_zero(info->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;
    {
        static int reporting;       /* a fault while reporting must not loop */
        if (reporting)
            ExitProcess(4);
        reporting = 1;
    }
    fprintf(stderr, "crash: exception %08lX at eip %08lX", record->ExceptionCode, info->ContextRecord->Eip);
    if (record->NumberParameters >= 2)
        fprintf(stderr, ", %s address %08lX", record->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long)record->ExceptionInformation[1]);
    fprintf(stderr, "\nstack words that point into the program:");
    for (i = 0; i < 400 && shown < 12; i++) {
        if (IsBadReadPtr(stack + i, 4))
            break;
        if (stack[i] >= 0x01800000u && stack[i] < 0x02000000u) {
            fprintf(stderr, " %08lX", (unsigned long)stack[i]);
            shown++;
        }
    }
    fprintf(stderr, "\n%d distinct stub(s) had been reached\n", ss_stub_count());
    fflush(stderr);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Started without a ROM argument (a double click): use the ROM remembered from last time, or ask for one. */
static const char *find_rom(void)
{
    static char path[MAX_PATH], remembered[MAX_PATH];
    OPENFILENAMEA dialog;
    FILE *file;
    char *slash;

    GetModuleFileNameA(NULL, remembered, sizeof remembered);
    slash = strrchr(remembered, '\\');
    strcpy(slash ? slash + 1 : remembered, "rom_path.txt");
    file = fopen(remembered, "r");
    if (file) {
        if (fgets(path, sizeof path, file))
            path[strcspn(path, "\r\n")] = 0;
        fclose(file);
        if (path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            return path;
    }
    path[0] = 0;
    memset(&dialog, 0, sizeof dialog);
    dialog.lStructSize = sizeof dialog;
    dialog.lpstrFilter = "Game Boy Advance ROM (*.gba)\0*.gba\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof path;
    dialog.lpstrTitle = "ShiningSource: choose your Shining Soul II ROM";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog))
        return NULL;
    file = fopen(remembered, "w");
    if (file) {
        fprintf(file, "%s\n", path);
        fclose(file);
    }
    return path;
}

int main(int argc, char **argv)
{
    const char *error = "", *rom = argc >= 2 ? argv[1] : find_rom();
    char message[600];

    AddVectoredExceptionHandler(1, report_crash);
    if (getenv("SS_WATCH")) {           /* debugging aid: hardware write watchpoint on a 4-byte address */
        CONTEXT context;
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(GetCurrentThread(), &context);
        context.Dr0 = strtoul(getenv("SS_WATCH"), NULL, 16);
        context.Dr7 = 0x000D0001;       /* enable breakpoint 0, on write, 4 bytes */
        SetThreadContext(GetCurrentThread(), &context);
        watch_from = getenv("SS_WATCH_FROM") ? strtoul(getenv("SS_WATCH_FROM"), NULL, 10) : 0;
    }
    if (!rom)
        return 2;
    if (ss_memory_init(rom, &error) != 0) {
        fprintf(stderr, "memory setup failed: %s\n", error);
        snprintf(message, sizeof message, "Could not start with this ROM:\n%s\n\n%s", rom, error);
        MessageBoxA(NULL, message, "ShiningSource", MB_ICONERROR);
        return 1;
    }
    fprintf(stderr, "%u code pointers in the ROM data now point to native functions\n", ss_patch_code_pointers());
    fprintf(stderr, "memory mapped, ROM loaded; entering the game's AgbMain\n");
    AgbMain();
    fprintf(stderr, "AgbMain returned; %d distinct stub(s) reached\n", ss_stub_count());
    return 0;
}
