/* 64-bit host for Windows.
 *
 * The game is a separate image: 64-bit instructions, 32-bit pointers, linked at a fixed low address. This
 * program maps the GBA address space at its hardware addresses, loads the ROM and the image, and gives the
 * image a table of services (bridge.h). The image runs on stacks in low memory, because it stores addresses of
 * its locals in 32 bits.
 *
 * Exit codes before the game starts: 2 arguments or files, 3 a needed low address range is taken, 4 image.
 */
#include "../guest/bridge.h"
#include "../achievements/ach_host.h"
#include "shiningsource/runtime.h"

#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>

#define SYSV __attribute__((sysv_abi))
#define PTR(a) ((void *)(uintptr_t)(uint32_t)(a))

static uint32_t image_low, image_high;

/* ---- low memory ---- */

/* Asking the system for fixed low addresses at run time is not reliable: something may already be mapped
 * there. So the host is linked at a low base and carries one large zero-filled block. The image of a program
 * is mapped before anything else in a new process, which guarantees the block. The GBA regions, the game image
 * and its stacks are all addresses inside it.
 *
 * The block is a read-only section: Windows charges memory for the writable pages of a program up front, and
 * 224 MiB of them would be committed although the game touches about a quarter. Each range is made writable
 * when it is handed out (low_alloc), and only then counts. */
/* "bw": no bytes in the file; the linker then marks the section read-only (tools/build_win64.py) */
__asm__(".section .arena,\"bw\"\n"
        ".globl ss_arena\n"
        ".p2align 16\n"
        "ss_arena:\n"
        ".space 0x0E000000\n"
        ".text\n");
extern char ss_arena[0x0E000000];
enum { ARENA_FROM = 0x02000000, ARENA_TO = 0x0F400000 };

static void *low_alloc(uint32_t address, uint32_t size, DWORD protection)
{
    DWORD old;
    if (address < ARENA_FROM || address + size > ARENA_TO)
        return NULL;
    if (!VirtualProtect(PTR(address), size, protection, &old))
        return NULL;
    return PTR(address);
}

/* Stacks for the image: a reserved low range handed out in fixed slots. */
enum { STACK_SLOT = 0x80000, STACK_SLOTS = 128 };
static const uint32_t stack_arena = 0x0B000000u, main_stack = 0x0F000000u;
static uint8_t stack_used[STACK_SLOTS];

static SYSV uint32_t b_stack_alloc(uint32_t size)
{
    int i;
    if (size > STACK_SLOT - 0x2000)
        return 0;
    for (i = 0; i < STACK_SLOTS; i++) {
        if (!stack_used[i]) {
            if (!low_alloc(stack_arena + (uint32_t)i * STACK_SLOT, STACK_SLOT, PAGE_READWRITE))
                return 0;
            stack_used[i] = 1;
            return stack_arena + (uint32_t)i * STACK_SLOT;
        }
    }
    return 0;
}

static SYSV void b_stack_free(uint32_t base, uint32_t size)
{
    (void)size;
    stack_used[(base - stack_arena) / STACK_SLOT] = 0;
}

static SYSV void b_stack_bounds(uint32_t low, uint32_t high)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackBase = PTR(high);
    tib->StackLimit = PTR(low);
}

/* ---- files, environment, log ---- */

static FILE *files[32];

static SYSV int b_write(int handle, uint32_t data, uint32_t size)
{
    FILE *file = handle == 1 ? stdout : handle == 2 ? stderr : (handle >= 3 && handle < 32) ? files[handle] : NULL;
    int put = file ? (int)fwrite(PTR(data), 1, size, file) : -1;
    if (handle <= 2 && file)
        fflush(file);
    return put;
}

static SYSV int b_open(uint32_t path, uint32_t mode)
{
    int i;
    for (i = 3; i < 32; i++) {
        if (!files[i]) {
            files[i] = fopen(PTR(path), PTR(mode));
            return files[i] ? i : 0;
        }
    }
    return 0;
}

static SYSV int b_read(int handle, uint32_t data, uint32_t size)
{
    return (handle >= 3 && handle < 32 && files[handle]) ? (int)fread(PTR(data), 1, size, files[handle]) : -1;
}

static SYSV int b_close(int handle)
{
    int result = -1;
    if (handle >= 3 && handle < 32 && files[handle]) {
        result = fclose(files[handle]);
        files[handle] = NULL;
    }
    return result;
}

static SYSV int b_remove(uint32_t path) { return remove(PTR(path)); }
static SYSV int b_rename(uint32_t from, uint32_t to) { return rename(PTR(from), PTR(to)); }

static SYSV int b_getenv(uint32_t name, uint32_t out, uint32_t capacity)
{
    const char *value = getenv(PTR(name));
    size_t length;
    if (!value)
        return -1;
    length = strlen(value);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(PTR(out), value, length);
    ((char *)PTR(out))[length] = 0;
    return (int)length;
}

static SYSV void b_exit(int code) { exit(code); }
static SYSV uint32_t b_version(void) { return SS_BRIDGE_VERSION; }
static SYSV int b_video_init(uint32_t title) { return ss_plat_video_init(PTR(title)); }
static void sign_of_life(void);
static volatile LONG alive;
/* Tests of the reports themselves: SS_TEST_FAULT=crash or hang, three seconds into the session. */
static void test_fault(void)
{
    static int frames, kind = -1;
    if (kind < 0) {
        const char *text = getenv("SS_TEST_FAULT");
        kind = !text ? 0 : !strcmp(text, "crash") ? 1 : !strcmp(text, "hang") ? 2 : 0;
    }
    if (!kind || ++frames < 180)
        return;
    if (kind == 1)
        *(volatile int *)(uintptr_t)8 = 0;
    for (;;)
        Sleep(100);
}

static SYSV uint32_t b_present(uint32_t pixels, int width)
{
    sign_of_life();
    test_fault();
    return ss_plat_present(PTR(pixels), width);
}
static SYSV int b_view_width(void) { return ss_plat_view_width(); }
static SYSV void b_audio(uint32_t samples, int frames) { ss_plat_audio(PTR(samples), frames); }
static SYSV double b_pow(double a, double b) { return pow(a, b); }
static SYSV double b_sin(double a) { return sin(a); }
static SYSV double b_cos(double a) { return cos(a); }
static SYSV double b_fmod(double a, double b) { return fmod(a, b); }
static SYSV void b_pump(void) { ss_plat_pump(); }
static SYSV void b_overlay(int darkness, uint32_t text) { ss_plat_overlay(darkness, text ? PTR(text) : NULL); }
static SYSV void b_status(uint32_t text) { ss_plat_status(text ? PTR(text) : NULL); }
static SYSV int b_net_open(uint32_t host, int port) { return ss_plat_net_open(PTR(host), port); }
static SYSV int b_net_send(uint32_t data, int size) { return ss_plat_net_send(PTR(data), size); }
static SYSV int b_net_recv(uint32_t data, int capacity) { return ss_plat_net_recv(PTR(data), capacity); }
static SYSV void b_net_wait(int milliseconds) { InterlockedIncrement(&alive); ss_plat_net_wait(milliseconds); }
static SYSV void b_net_farewell(uint32_t data, int size) { ss_plat_net_farewell(data ? PTR(data) : NULL, size); }
static SYSV uint32_t b_ticks(void) { return ss_plat_ticks_ms(); }
static SYSV void b_achievements(int run_flags) { ss_ach_frame(run_flags); }

static uint32_t bridge[SS_B_COUNT];
#define ENTRY(index, function) bridge[index] = (uint32_t)(uintptr_t)(function)

/* ---- accesses the console would have ignored: the decoder is shared with the Linux host ---- */

#include "stray64.h"
#define tolerated ss_tolerated

static void cpu_of(CONTEXT *c, struct ss_cpu *cpu)
{
    DWORD64 *registers[16] = { &c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                               &c->R8, &c->R9, &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15 };
    int i;
    for (i = 0; i < 16; i++)
        cpu->reg[i] = (uint64_t *)registers[i];
    cpu->rip = (uint64_t *)&c->Rip;
    cpu->flags = c->EFlags;
}

static int stray_access(CONTEXT *c, uint64_t address, int is_write)
{
    struct ss_cpu cpu;
    int handled;
    cpu_of(c, &cpu);
    handled = ss_stray_access64(&cpu, address, is_write, SS_EWRAM, SS_ROM + 0x1000000);
    c->EFlags = (DWORD)cpu.flags;
    return handled;
}

static int divide_by_zero(CONTEXT *c)
{
    struct ss_cpu cpu;
    cpu_of(c, &cpu);
    return ss_divide_by_zero64(&cpu);
}

/* ---- reports that survive the session ----
 * A crash or a hang is written to the console and appended to shiningsource_crash.log next to the program
 * (the launcher starts the game without a console anyone reads). The file is bounded: at 256 KiB it becomes
 * shiningsource_crash.old.log and a new one starts. Nothing personal is written: addresses, registers, counters. */
static char report_path[MAX_PATH];

static void report(const char *format, ...)
{
    char line[512];
    va_list arguments;
    FILE *file;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    fputs(line, stderr);
    fflush(stderr);
    if (report_path[0] && (file = fopen(report_path, "a")) != NULL) {
        fputs(line, file);
        fclose(file);
    }
}

static void report_start(const char *what)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    SYSTEMTIME now;
    if (!report_path[0]) {
        char *slash;
        GetModuleFileNameA(NULL, report_path, sizeof report_path - 32);
        slash = strrchr(report_path, '\\');
        strcpy(slash ? slash + 1 : report_path, "shiningsource_crash.log");
    }
    if (GetFileAttributesExA(report_path, GetFileExInfoStandard, &data) && data.nFileSizeLow > 256 * 1024) {
        char old[MAX_PATH];
        snprintf(old, sizeof old, "%.*s.old.log", (int)strlen(report_path) - 4, report_path);
        MoveFileExA(report_path, old, MOVEFILE_REPLACE_EXISTING);
    }
    GetSystemTime(&now);
    report("---- %s, %04d-%02d-%02d %02d:%02d:%02d UTC, host built " __DATE__ " " __TIME__ " ----\n", what,
           now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}

static void report_context(const CONTEXT *c)
{
    const uint64_t *stack = (const uint64_t *)c->Rsp;
    int i, shown = 0;
    report("rax %016llX rbx %016llX rcx %016llX rdx %016llX\nrsi %016llX rdi %016llX rbp %016llX rsp %016llX\n",
           c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    report("stack words that point into the game image:");
    for (i = 0; i < 600 && shown < 12 && !IsBadReadPtr(stack + i, 8); i++) {
        if (stack[i] >= image_low && stack[i] < image_high) {
            report(" %08llX", (unsigned long long)stack[i]);
            shown++;
        }
    }
    report("\n%lu stray accesses had been tolerated\n", tolerated);
}

/* ---- hangs ----
 * Every frame shown and every wait on the network counts as a sign of life. Once the game has shown a frame,
 * ten seconds without one means it is stuck: the watchdog writes where the game's thread is, once per hang,
 * and lets it go on (it may only be very slow). The window stays in the user's hands either way. */
static HANDLE game_thread;

static DWORD WINAPI watchdog(LPVOID unused)
{
    LONG seen = alive;
    int quiet = 0, told = 0;
    (void)unused;
    for (;;) {
        Sleep(1000);
        if (alive != seen) {
            seen = alive;
            quiet = told = 0;
            continue;
        }
        if (++quiet >= 10 && !told) {
            CONTEXT context;
            told = 1;
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(game_thread) != (DWORD)-1) {
                if (GetThreadContext(game_thread, &context)) {
                    report_start("hang");
                    report("no frame for %d seconds; the game's thread is at rip %016llX (%s)\n", quiet, (unsigned long long)context.Rip,
                           context.Rip >= image_low && context.Rip < image_high ? "game image" : "host or system");
                    report_context(&context);
                }
                ResumeThread(game_thread);
            }
        }
    }
}

static void sign_of_life(void)
{
    if (!game_thread && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                                        DUPLICATE_SAME_ACCESS))
        CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    InterlockedIncrement(&alive);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    CONTEXT *c = info->ContextRecord;
    int in_image = c->Rip >= image_low && c->Rip < image_high;
    static int reporting;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION
        && record->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && record->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (in_image && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2
        && stray_access(c, record->ExceptionInformation[1], record->ExceptionInformation[0] != 0))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (in_image && record->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO && divide_by_zero(c))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (reporting)
        ExitProcess(4);
    reporting = 1;
    report_start("crash");
    report("exception %08lX at rip %016llX (%s)", record->ExceptionCode, (unsigned long long)c->Rip,
           in_image ? "game image" : "host");
    if (record->NumberParameters >= 2)
        report(", %s address %016llX", record->ExceptionInformation[0] ? "writing" : "reading",
               (unsigned long long)record->ExceptionInformation[1]);
    report("\n");
    report_context(c);
    fflush(stderr);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- loading ---- */

static int load_file(const char *path, uint32_t address, uint32_t capacity)
{
    FILE *file = fopen(path, "rb");
    size_t got;
    if (!file)
        return -1;
    got = fread(PTR(address), 1, capacity, file);
    fclose(file);
    return (int)got;
}

/* ELF32 for x86-64 (the x32 ABI): copy each PT_LOAD segment to its address. Returns the entry point. */
static uint32_t load_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    uint8_t header[52], program[32];
    uint32_t entry, offset, low = ~0u, high = 0;
    int count, size, i;

    if (!file)
        return 0;
    if (fread(header, 1, 52, file) != 52 || memcmp(header, "\x7f" "ELF\x01", 5) != 0) {
        fclose(file);
        return 0;
    }
    memcpy(&entry, header + 0x18, 4);
    memcpy(&offset, header + 0x1C, 4);
    size = header[0x2A] | header[0x2B] << 8;
    count = header[0x2C] | header[0x2D] << 8;
    for (i = 0; i < count; i++) {           /* first pass: the range of the whole image */
        uint32_t type, address, memory;
        fseek(file, (long)(offset + (uint32_t)(i * size)), SEEK_SET);
        fread(program, 1, 32, file);
        memcpy(&type, program, 4); memcpy(&address, program + 8, 4); memcpy(&memory, program + 20, 4);
        if (type != 1 || !memory) continue;
        if (address < low) low = address;
        if (address + memory > high) high = address + memory;
    }
    low &= ~0xFFFFu;
    high = (high + 0xFFFFu) & ~0xFFFFu;
    if (low >= high || !low_alloc(low, high - low, PAGE_EXECUTE_READWRITE)) {
        fclose(file);
        return 0;
    }
    for (i = 0; i < count; i++) {
        uint32_t type, file_offset, address, file_size;
        fseek(file, (long)(offset + (uint32_t)(i * size)), SEEK_SET);
        fread(program, 1, 32, file);
        memcpy(&type, program, 4); memcpy(&file_offset, program + 4, 4);
        memcpy(&address, program + 8, 4); memcpy(&file_size, program + 16, 4);
        if (type != 1) continue;
        fseek(file, (long)file_offset, SEEK_SET);
        fread(PTR(address), 1, file_size, file);
    }
    fclose(file);
    image_low = low;
    image_high = high;
    return entry;
}

static const char *find_rom(char *path, size_t capacity)
{
    char remembered[MAX_PATH];
    OPENFILENAMEA dialog;
    FILE *file;
    char *slash;

    GetModuleFileNameA(NULL, remembered, sizeof remembered);
    slash = strrchr(remembered, '\\');
    strcpy(slash ? slash + 1 : remembered, "rom_path.txt");
    file = fopen(remembered, "r");
    if (file) {
        if (fgets(path, (int)capacity, file))
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
    dialog.nMaxFile = (DWORD)capacity;
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

/* Run the image's entry on its own stack. Microsoft x64 in, System V out. */
void ss_enter_image(uint64_t entry, uint64_t stack_top, uint64_t bridge_table, uint64_t stack_low);
__asm__(
    ".text\n"
    ".globl ss_enter_image\n"
    "ss_enter_image:\n"
    "    movq %rdx, %rsp\n"            /* never returns: the image ends through the exit service */
    "    movl %r8d, %edi\n"            /* System V argument 1: the bridge table */
    "    movl %r9d, %esi\n"            /* argument 2: lowest address of this stack */
    "    movl %edx, %edx\n"            /* argument 3: its top */
    "    callq *%rcx\n"
    "    ud2\n");

static int fail(int code, const char *text)
{
    fprintf(stderr, "%s\n", text);
    MessageBoxA(NULL, text, "ShiningSource", MB_ICONERROR);
    return code;
}

int main(int argc, char **argv)
{
    static const struct { uint32_t base, size; const char *name; } regions[] = {
        { SS_EWRAM, 0x40000, "EWRAM" }, { SS_IWRAM, 0x10000, "IWRAM" }, { SS_IO, 0x10000, "I/O registers" },
        { SS_PALETTE, 0x10000, "palette" }, { SS_VRAM, 0x20000, "VRAM" }, { SS_OAM, 0x10000, "OAM" },
        { SS_ROM, 0x1000000, "ROM" },
    };
    char image_path[MAX_PATH], rom_buffer[MAX_PATH], message[400];
    const char *rom = argc >= 2 ? argv[1] : find_rom(rom_buffer, sizeof rom_buffer);
    uint32_t entry, stack;
    char *slash;
    unsigned i;
    int rom_size;

    if (!rom)
        return 2;
    AddVectoredExceptionHandler(1, on_fault);
    for (i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        if (!low_alloc(regions[i].base, regions[i].size, PAGE_READWRITE)) {
            snprintf(message, sizeof message, "The address range the game needs for its %s (0x%08X) is not free in this process.",
                     regions[i].name, (unsigned)regions[i].base);
            return fail(3, message);
        }
    }
    if ((uintptr_t)ss_arena > ARENA_FROM || (uintptr_t)ss_arena + sizeof ss_arena < ARENA_TO)
        return fail(3, "The host was not linked so that its arena covers the game's address space.");
    rom_size = load_file(rom, SS_ROM, 0x1000000);
    if (rom_size < 0x100000)
        return fail(2, "The ROM file could not be read.");
    GetModuleFileNameA(NULL, image_path, sizeof image_path);
    slash = strrchr(image_path, '\\');
    strcpy(slash ? slash + 1 : image_path, argc >= 3 ? argv[2] : "ss2_game.elf");
    entry = load_image(argc >= 3 && strchr(argv[2], ':') ? argv[2] : image_path);
    if (!entry)
        return fail(4, "The game image (ss2_game.elf) is missing, damaged, or its address range is taken.");

    ENTRY(SS_B_VERSION, b_version); ENTRY(SS_B_WRITE, b_write); ENTRY(SS_B_OPEN, b_open); ENTRY(SS_B_READ, b_read);
    ENTRY(SS_B_CLOSE, b_close); ENTRY(SS_B_REMOVE, b_remove); ENTRY(SS_B_RENAME, b_rename); ENTRY(SS_B_GETENV, b_getenv);
    ENTRY(SS_B_EXIT, b_exit); ENTRY(SS_B_VIDEO_INIT, b_video_init); ENTRY(SS_B_PRESENT, b_present); ENTRY(SS_B_AUDIO, b_audio);
    ENTRY(SS_B_STACK_ALLOC, b_stack_alloc); ENTRY(SS_B_STACK_FREE, b_stack_free); ENTRY(SS_B_STACK_BOUNDS, b_stack_bounds);
    ENTRY(SS_B_VIEW_WIDTH, b_view_width); ENTRY(SS_B_POW, b_pow); ENTRY(SS_B_SIN, b_sin); ENTRY(SS_B_COS, b_cos); ENTRY(SS_B_FMOD, b_fmod);
    ENTRY(SS_B_PUMP, b_pump); ENTRY(SS_B_NET_OPEN, b_net_open); ENTRY(SS_B_NET_SEND, b_net_send); ENTRY(SS_B_NET_RECV, b_net_recv);
    ENTRY(SS_B_NET_WAIT, b_net_wait); ENTRY(SS_B_NET_FAREWELL, b_net_farewell); ENTRY(SS_B_TICKS, b_ticks); ENTRY(SS_B_STATUS, b_status); ENTRY(SS_B_OVERLAY, b_overlay);
    ENTRY(SS_B_ACHIEVEMENTS, b_achievements);
    if ((uintptr_t)bridge >> 32 || (uintptr_t)b_version >> 32)
        return fail(4, "The host was not linked below 4 GiB.");

    /* achievements start here, on the program's own stack: reading the remembered sign-in asks the system */
    ss_ach_start(PTR(SS_ROM), (size_t)rom_size);

    stack = main_stack;
    if (!low_alloc(main_stack, 0x400000, PAGE_READWRITE))
        return fail(3, "The game's main stack could not be set up.");
    fprintf(stderr, "host64: GBA memory mapped, ROM and image loaded (image %08X-%08X, entry %08X)\n",
            (unsigned)image_low, (unsigned)image_high, (unsigned)entry);
    b_stack_bounds(stack, stack + 0x400000);
    ss_enter_image(entry, stack + 0x400000, (uintptr_t)bridge, stack);
    return 0;
}
