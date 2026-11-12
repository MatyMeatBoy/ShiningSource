/* 64-bit host for Linux (x86-64). Same job as the Windows host (host_main.c): map the GBA address space, load
 * the ROM and the game image, serve the bridge. The image file is the same one the Windows host loads.
 *
 * Link it as a non-PIE program so that its functions have addresses below 4 GiB: the image calls the services
 * through 32-bit pointers.
 */
#define _GNU_SOURCE
#include "../guest/bridge.h"
#include "shiningsource/runtime.h"
#include "stray64.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define PTR(a) ((void *)(uintptr_t)(uint32_t)(a))
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint32_t image_low, image_high;

static void *low_map(uint32_t address, uint32_t size, int protection)
{
    void *got = mmap(PTR(address), size, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != PTR(address)) {
        if (got != MAP_FAILED)
            munmap(got, size);
        return NULL;
    }
    return got;
}

enum { STACK_SLOT = 0x80000, STACK_SLOTS = 128 };
static const uint32_t stack_arena = 0x0B000000u, main_stack = 0x0F000000u;
static uint8_t stack_used[STACK_SLOTS];

static uint32_t b_stack_alloc(uint32_t size)
{
    int i;
    if (size > STACK_SLOT - 0x2000)
        return 0;
    for (i = 0; i < STACK_SLOTS; i++) {
        if (!stack_used[i]) {
            stack_used[i] = 1;
            return stack_arena + (uint32_t)i * STACK_SLOT;
        }
    }
    return 0;
}

static void b_stack_free(uint32_t base, uint32_t size) { (void)size; stack_used[(base - stack_arena) / STACK_SLOT] = 0; }
static void b_stack_bounds(uint32_t low, uint32_t high) { (void)low; (void)high; }      /* nothing to tell Linux */

static FILE *files[32];

static int b_write(int handle, uint32_t data, uint32_t size)
{
    FILE *file = handle == 1 ? stdout : handle == 2 ? stderr : (handle >= 3 && handle < 32) ? files[handle] : NULL;
    int put = file ? (int)fwrite(PTR(data), 1, size, file) : -1;
    if (handle <= 2 && file)
        fflush(file);
    return put;
}

static int b_open(uint32_t path, uint32_t mode)
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

static int b_read(int handle, uint32_t data, uint32_t size)
{
    return (handle >= 3 && handle < 32 && files[handle]) ? (int)fread(PTR(data), 1, size, files[handle]) : -1;
}

static int b_close(int handle)
{
    int result = -1;
    if (handle >= 3 && handle < 32 && files[handle]) {
        result = fclose(files[handle]);
        files[handle] = NULL;
    }
    return result;
}

static int b_remove(uint32_t path) { return remove(PTR(path)); }
static int b_rename(uint32_t from, uint32_t to) { return rename(PTR(from), PTR(to)); }

static int b_getenv(uint32_t name, uint32_t out, uint32_t capacity)
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

static void b_exit(int code) { exit(code); }
static uint32_t b_version(void) { return SS_BRIDGE_VERSION; }
static int b_video_init(uint32_t title) { return ss_plat_video_init(PTR(title)); }
static uint32_t b_present(uint32_t pixels, int width) { return ss_plat_present(PTR(pixels), width); }
static int b_view_width(void) { return ss_plat_view_width(); }
static void b_audio(uint32_t samples, int frames) { ss_plat_audio(PTR(samples), frames); }
static double b_pow(double a, double b) { return pow(a, b); }
static double b_sin(double a) { return sin(a); }
static double b_cos(double a) { return cos(a); }
static double b_fmod(double a, double b) { return fmod(a, b); }

static uint32_t bridge[SS_B_COUNT];
#define ENTRY(index, function) bridge[index] = (uint32_t)(uintptr_t)(function)

/* SIGSEGV / SIGFPE raised by game code: absorb what the console would have ignored, report the rest. */
static void on_fault(int number, siginfo_t *info, void *raw)
{
    ucontext_t *context = raw;
    greg_t *g = context->uc_mcontext.gregs;
    static const int order[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                   REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
    struct ss_cpu cpu;
    int i, in_image = (uint64_t)g[REG_RIP] >= image_low && (uint64_t)g[REG_RIP] < image_high, handled = 0;

    for (i = 0; i < 16; i++)
        cpu.reg[i] = (uint64_t *)&g[order[i]];
    cpu.rip = (uint64_t *)&g[REG_RIP];
    cpu.flags = (uint64_t)g[REG_EFL];
    if (in_image && number == SIGSEGV)
        handled = ss_stray_access64(&cpu, (uint64_t)(uintptr_t)info->si_addr, (g[REG_ERR] & 2) != 0, SS_EWRAM, SS_ROM + 0x1000000);
    else if (in_image && number == SIGFPE)
        handled = ss_divide_by_zero64(&cpu);
    if (handled) {
        g[REG_EFL] = (greg_t)cpu.flags;
        return;
    }
    fprintf(stderr, "crash: signal %d at rip %016llx (%s), address %p; %lu stray accesses had been tolerated\n", number,
            (unsigned long long)g[REG_RIP], in_image ? "game image" : "host", info->si_addr, ss_tolerated);
    _exit(3);
}

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
    for (i = 0; i < count; i++) {
        uint32_t type, address, memory;
        fseek(file, (long)(offset + (uint32_t)(i * size)), SEEK_SET);
        if (fread(program, 1, 32, file) != 32) break;
        memcpy(&type, program, 4); memcpy(&address, program + 8, 4); memcpy(&memory, program + 20, 4);
        if (type != 1 || !memory) continue;
        if (address < low) low = address;
        if (address + memory > high) high = address + memory;
    }
    low &= ~0xFFFFu;
    high = (high + 0xFFFFu) & ~0xFFFFu;
    if (low >= high || !low_map(low, high - low, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        fclose(file);
        return 0;
    }
    for (i = 0; i < count; i++) {
        uint32_t type, file_offset, address, file_size;
        fseek(file, (long)(offset + (uint32_t)(i * size)), SEEK_SET);
        if (fread(program, 1, 32, file) != 32) break;
        memcpy(&type, program, 4); memcpy(&file_offset, program + 4, 4);
        memcpy(&address, program + 8, 4); memcpy(&file_size, program + 16, 4);
        if (type != 1) continue;
        fseek(file, (long)file_offset, SEEK_SET);
        if (fread(PTR(address), 1, file_size, file) != file_size) { fclose(file); return 0; }
    }
    fclose(file);
    image_low = low;
    image_high = high;
    return entry;
}

/* Run the image's entry on its own low stack. Never returns: the image ends through the exit service. */
void ss_enter_image(uint64_t entry, uint64_t stack_top, uint64_t bridge_table, uint64_t stack_low);
__asm__(
    ".text\n"
    ".globl ss_enter_image\n"
    "ss_enter_image:\n"
    "    movq %rsi, %rsp\n"            /* System V in: rdi entry, rsi stack top, rdx bridge, rcx stack low */
    "    movq %rdi, %rax\n"
    "    movl %edx, %edi\n"            /* out: edi bridge, esi stack low, edx stack top */
    "    movl %esi, %edx\n"
    "    movl %ecx, %esi\n"
    "    callq *%rax\n"
    "    ud2\n");

int main(int argc, char **argv)
{
    static const struct { uint32_t base, size; const char *name; } regions[] = {
        { SS_EWRAM, 0x40000, "EWRAM" }, { SS_IWRAM, 0x10000, "IWRAM" }, { SS_IO, 0x10000, "I/O registers" },
        { SS_PALETTE, 0x10000, "palette" }, { SS_VRAM, 0x20000, "VRAM" }, { SS_OAM, 0x10000, "OAM" },
        { SS_ROM, 0x1000000, "ROM" },
    };
    static uint8_t signal_stack[1 << 16];
    struct sigaction action;
    stack_t alternate;
    char image_path[4096];
    const char *rom = argc >= 2 ? argv[1] : getenv("SHININGSOURCE_ROM");
    uint32_t entry;
    ssize_t length;
    unsigned i;

    if (!rom) {
        fprintf(stderr, "usage: %s ROM.gba [game image]   (or set SHININGSOURCE_ROM)\n", argv[0]);
        return 2;
    }
    for (i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        if (!low_map(regions[i].base, regions[i].size, PROT_READ | PROT_WRITE)) {
            fprintf(stderr, "the address range for the game's %s (0x%08X) is not free; check vm.mmap_min_addr\n",
                    regions[i].name, (unsigned)regions[i].base);
            return 3;
        }
    }
    if (!low_map(stack_arena, STACK_SLOT * STACK_SLOTS, PROT_READ | PROT_WRITE) || !low_map(main_stack, 0x400000, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "the address range for the game's stacks is not free\n");
        return 3;
    }
    if (load_file(rom, SS_ROM, 0x1000000) < 0x100000) {
        fprintf(stderr, "the ROM file could not be read: %s\n", rom);
        return 2;
    }
    if (argc >= 3) {
        snprintf(image_path, sizeof image_path, "%s", argv[2]);
    } else {
        char *slash;
        length = readlink("/proc/self/exe", image_path, sizeof image_path - 32);
        image_path[length > 0 ? length : 0] = 0;
        slash = strrchr(image_path, '/');
        strcpy(slash ? slash + 1 : image_path, "ss2_game.elf");
    }
    entry = load_image(image_path);
    if (!entry) {
        fprintf(stderr, "the game image is missing, damaged, or its address range is taken: %s\n", image_path);
        return 4;
    }
    alternate.ss_sp = signal_stack;
    alternate.ss_size = sizeof signal_stack;
    alternate.ss_flags = 0;
    sigaltstack(&alternate, NULL);
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    sigaction(SIGBUS, &action, NULL);

    ENTRY(SS_B_VERSION, b_version); ENTRY(SS_B_WRITE, b_write); ENTRY(SS_B_OPEN, b_open); ENTRY(SS_B_READ, b_read);
    ENTRY(SS_B_CLOSE, b_close); ENTRY(SS_B_REMOVE, b_remove); ENTRY(SS_B_RENAME, b_rename); ENTRY(SS_B_GETENV, b_getenv);
    ENTRY(SS_B_EXIT, b_exit); ENTRY(SS_B_VIDEO_INIT, b_video_init); ENTRY(SS_B_PRESENT, b_present); ENTRY(SS_B_AUDIO, b_audio);
    ENTRY(SS_B_STACK_ALLOC, b_stack_alloc); ENTRY(SS_B_STACK_FREE, b_stack_free); ENTRY(SS_B_STACK_BOUNDS, b_stack_bounds);
    ENTRY(SS_B_VIEW_WIDTH, b_view_width); ENTRY(SS_B_POW, b_pow); ENTRY(SS_B_SIN, b_sin); ENTRY(SS_B_COS, b_cos); ENTRY(SS_B_FMOD, b_fmod);
    if ((uintptr_t)bridge >> 32 || (uintptr_t)b_version >> 32) {
        fprintf(stderr, "the host must be linked as a non-PIE program (its code has to live below 4 GiB)\n");
        return 4;
    }
    fprintf(stderr, "host64: GBA memory mapped, ROM and image loaded (image %08X-%08X, entry %08X)\n",
            (unsigned)image_low, (unsigned)image_high, (unsigned)entry);
    ss_enter_image(entry, main_stack + 0x400000, (uintptr_t)bridge, main_stack);
    return 0;
}
