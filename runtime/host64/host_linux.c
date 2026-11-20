/* 64-bit host for Linux and Android, x86-64 and ARM64. Same job as the Windows host (host_main.c): map the GBA address
 * space, load the ROM and the game image, serve the bridge.
 *
 * x86-64: the image is the ELF file the Windows host loads. Link the host as a non-PIE program so that its
 * functions have addresses below 4 GiB: the image calls the services through 32-bit pointers.
 * ARM64: the image is the Mach-O file of tools/build_arm64.py, and the services are reached through small
 * jumps placed in low memory, so the host can be loaded anywhere.
 * Android: the host is the library SDL's activity loads (libmain.so), so it is never below 4 GiB and always
 * uses the jumps. The image comes from the APK's assets, the ROM and the saves live in the app's own folder,
 * and GBA memory is rebased inside the image: Java may own its original hardware addresses.
 */
#define _GNU_SOURCE
#include "../guest/bridge.h"
#include "shiningsource/runtime.h"
#ifndef __ANDROID__
#include <SDL3/SDL_stdinc.h>
#endif
#ifdef __x86_64__
#include "stray64.h"
#else
static unsigned long ss_tolerated;
#endif

#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#endif

#define PTR(a) ((void *)(uintptr_t)(uint32_t)(a))
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint32_t image_low, image_high, image_slide;

#ifdef __ANDROID__
/* Claim only the linked image window before graphics initialization. Never
 * assume the GBA's hardware addresses are free in a process inherited from ART. */
#ifdef SS_REBASED_HOST
enum { LOW_BASE = 0x0A000000u, LOW_END = 0x0C800000u };
#else
enum { LOW_BASE = 0x02000000u, LOW_END = 0x10000000u };
#endif
static int low_reserved;

__attribute__((constructor)) static void reserve_low_memory(void)
{
    void *got = mmap(PTR(LOW_BASE), LOW_END - LOW_BASE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (got == PTR(LOW_BASE))
        low_reserved = 1;
    else if (got != MAP_FAILED)
        munmap(got, LOW_END - LOW_BASE);
}
#else
enum { low_reserved = 0, LOW_BASE = 0, LOW_END = 0 };
#endif

#ifndef SS_REBASED_HOST
static void *low_map(uint32_t address, uint32_t size, int protection)
{
    void *got;
    if (low_reserved && address >= LOW_BASE && address + size <= LOW_END)       /* inside the claimed range */
        got = mmap(PTR(address), size, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    else {
        got = mmap(PTR(address), size, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        /* Kernels predating MAP_FIXED_NOREPLACE may reject it. A checked hint
         * is safe too: it never replaces somebody else's mapping. */
        if (got == MAP_FAILED && errno == EINVAL)
            got = mmap(PTR(address), size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (got != PTR(address)) {
        int reason = got == MAP_FAILED ? errno : EEXIST;
        if (got != MAP_FAILED)
            munmap(got, size);
        fprintf(stderr, "memory map %08X-%08X failed: %s (errno %d)\n",
                address, address + size, strerror(reason), reason);
        {
            FILE *maps = fopen("/proc/self/maps", "r");
            char line[1024];
            unsigned long long low, high;
            while (maps && fgets(line, sizeof line, maps))
                if (sscanf(line, "%llx-%llx", &low, &high) == 2 && low < (uint64_t)address + size && high > address)
                    fprintf(stderr, "overlapping mapping: %s", line);
            if (maps) fclose(maps);
        }
        errno = reason;
        return NULL;
    }
    return got;
}
#endif

enum { STACK_SLOT = 0x80000, STACK_SLOTS = 128 };
#ifdef SS_REBASED_HOST
static uint32_t stack_arena, main_stack;

/* Guest pointers (including SP and bridge entries) are 32 bits. A hint can
 * return somewhere else, so validate the complete range before narrowing it.
 * No MAP_FIXED: ART/firmware may already own any proposed address. */
static void *guest_map_at(uint64_t hint, uint32_t size, int protection, int *reason)
{
    void *got = mmap((void *)(uintptr_t)hint, size, protection,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got == MAP_FAILED) {
        *reason = errno;
        return NULL;
    }
    if ((uintptr_t)got >= 0x11000000u &&
        (uint64_t)(uintptr_t)got + size < UINT64_C(0x100000000)) {
        fprintf(stderr, "guest map %08X-%08X\n", (unsigned)(uintptr_t)got,
                (unsigned)((uintptr_t)got + size));
        return got;
    }
    munmap(got, size);
    *reason = ENOMEM;
    return NULL;
}

static void *guest_map(uint32_t size, int protection)
{
    int reason = ENOMEM;
    void *got = guest_map_at(0x20000000u, size, protection, &reason);
    FILE *maps;
    if (got)
        return got;
    maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[1024];
        unsigned long long low, high;
        uint64_t next = 0x11000000u;
        /* A coarse hint scan misses exact-size holes and gaps below its first
         * hint. Walk actual gaps; mmap remains non-replacing if maps race. */
        while (fgets(line, sizeof line, maps)) {
            if (sscanf(line, "%llx-%llx", &low, &high) != 2)
                continue;
            if (low > next && low - next >= size && next + size < UINT64_C(0x100000000)) {
                got = guest_map_at(next, size, protection, &reason);
                if (got)
                    break;
            }
            if (high > next)
                next = high;
            if (next + size >= UINT64_C(0x100000000))
                break;
        }
        if (!got && next + size < UINT64_C(0x100000000))
            got = guest_map_at(next, size, protection, &reason);
        fclose(maps);
    } else {
        /* Self maps are normally readable on Android. If restricted, probe
         * page-aligned hints so a sufficient gap cannot fall between them. */
        long page = sysconf(_SC_PAGESIZE);
        uint64_t hint;
        if (page <= 0)
            page = 0x10000;
        for (hint = 0x11000000u; hint + size < UINT64_C(0x100000000); hint += (unsigned long)page) {
            got = guest_map_at(hint, size, protection, &reason);
            if (got)
                break;
        }
    }
    if (got)
        return got;
    fprintf(stderr, "no free 32-bit guest range for %u bytes: %s (errno %d)\n",
            size, strerror(reason), reason);
    errno = reason;
    return NULL;
}
#else
static const uint32_t stack_arena = 0x0B000000u, main_stack = 0x0F000000u;
static const uint32_t image_limit = 0x0B000000u;
#endif
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
#ifdef __ANDROID__
    const char *value = SDL_getenv(PTR(name));      /* includes the SDL_ENV.* entries of the manifest */
#else
    const char *value = getenv(PTR(name));
#endif
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

#ifndef __ANDROID__
static char **restart_args;
#endif

static void b_exit(int code)
{
    int reported_code = code;
#ifdef __ANDROID__
    /* Android returns to the launcher on a soft reset; its exit is normal. */
    if (reported_code == SS_EXIT_RESTART)
        reported_code = 0;
#endif
    fprintf(stderr, "host64: exit code %d\n", reported_code);
    if (ss_tolerated)
        fprintf(stderr, "%lu stray accesses were tolerated\n", ss_tolerated);
#ifndef __ANDROID__
    if (code == SS_EXIT_RESTART && restart_args) {
        fflush(NULL);
        execv("/proc/self/exe", restart_args);
        perror("Could not restart ShiningSource");
        reported_code = 1;
    }
#endif
#ifdef __ANDROID__
    /* On Android this is the game's thread, and SDL's other threads are still running: exit() tears down what
     * they hold (a destroyed mutex is then locked and the process aborts on its way out). Everything of ours is
     * flushed here and the process ends at once. What the platform would have said at exit (SS_STATS) is said
     * first, since nothing registered with atexit runs. */
    {
        extern void ss_plat_last_words(void);
        ss_plat_last_words();
    }
    fflush(NULL);
    _exit(reported_code);
#endif
    exit(reported_code);
}
static uint32_t b_version(void) { return SS_BRIDGE_VERSION; }
static int b_video_init(uint32_t title) { return ss_plat_video_init(PTR(title)); }
static uint32_t b_present(uint32_t pixels, int width) { return ss_plat_present(PTR(pixels), width); }
static int b_view_width(void) { return ss_plat_view_width(); }
static void b_audio(uint32_t samples, int frames) { ss_plat_audio(PTR(samples), frames); }
static double b_pow(double a, double b) { return pow(a, b); }
static double b_sin(double a) { return sin(a); }
static double b_cos(double a) { return cos(a); }
static double b_fmod(double a, double b) { return fmod(a, b); }
static void b_pump(void) { ss_plat_pump(); }
static void b_overlay(int darkness, uint32_t text) { ss_plat_overlay(darkness, text ? PTR(text) : NULL); }
static void b_status(uint32_t text) { ss_plat_status(text ? PTR(text) : NULL); }
static int b_net_open(uint32_t host, int port) { return ss_plat_net_open(PTR(host), port); }
static int b_net_send(uint32_t data, int size) { return ss_plat_net_send(PTR(data), size); }
static int b_net_recv(uint32_t data, int capacity) { return ss_plat_net_recv(PTR(data), capacity); }
static void b_net_wait(int milliseconds) { ss_plat_net_wait(milliseconds); }
static void b_net_farewell(uint32_t data, int size) { ss_plat_net_farewell(data ? PTR(data) : NULL, size); }
static uint32_t b_ticks(void) { return ss_plat_ticks_ms(); }
/* Achievements are evaluated by the Windows host only so far (runtime/achievements); here the frame's call does
 * nothing. Cheats still work: they are in the game image (core/rebel.c). */
static void b_achievements(int run_flags) { (void)run_flags; }

/* The options menu (plat_sdl.c) is part of this program; what it switches is in the game image, which handed
 * over one function for it (bridge.h). These are the core's names for the same things, so the platform file is
 * the same whether it is linked with the core or with a host. */
static int (*guest_option)(int which, int value);
static void b_option_hook(uint32_t function) { guest_option = (int (*)(int, int))(uintptr_t)function; }
void ss_frame_set_smooth(int on) { if (guest_option) guest_option(SS_OPTION_SMOOTH, on); }
void ss_frame_set_view(int width) { if (guest_option) guest_option(SS_OPTION_VIEW, width); }
void ss_frame_set_hud(int on) { if (guest_option) guest_option(SS_OPTION_HUD, on); }
void ss_skip_set_enabled(int on) { if (guest_option) guest_option(SS_OPTION_SKIP, on); }
void ss_remote_sfx_set_enabled(int on)
{
    if (guest_option) {
        int actual = guest_option(SS_OPTION_REMOTE_SFX, on);
        if (getenv("SS_REMOTE_SFX_LOG"))
            fprintf(stderr, "remote sounds: enabled=%d\n", actual);
    }
}
int ss_net_active(void) { return guest_option ? guest_option(SS_OPTION_ONLINE, 0) : 0; }

static uint64_t bridge[SS_B_COUNT];
#define ENTRY(index, function) bridge[index] = (uint64_t)(uintptr_t)(function)
#ifdef __aarch64__
#define IMAGE_NAME "ss2_game.macho"
#else
#define IMAGE_NAME "ss2_game.elf"
#endif

#ifdef __x86_64__
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
    if (in_image)
        fprintf(stderr, "linked game pc %08X (slide %08X)\n", (unsigned)((uint32_t)g[REG_RIP] - image_slide), image_slide);
    _exit(3);
}

#else
/* ARM64: a load or store that hit unmapped memory. Loads give zero, stores are dropped, as on x86 (win32/stray.c
 * has the reasoning). Division by zero does not trap on this processor. */
static void on_fault(int number, siginfo_t *info, void *raw)
{
    ucontext_t *context = raw;
    uint64_t pc = context->uc_mcontext.pc, address = (uint64_t)(uintptr_t)info->si_addr;
    int in_image = pc >= image_low && pc < image_high;

    if (in_image && (number == SIGSEGV || number == SIGBUS) && !(address >= SS_EWRAM && address < SS_ROM + 0x1000000)) {
        uint32_t insn = *(const uint32_t *)(uintptr_t)pc;
        uint64_t *x = (uint64_t *)context->uc_mcontext.regs;
        int rt = insn & 31, rn = (insn >> 5) & 31, handled = 0;
        if ((insn & 0x3A000000u) == 0x28000000u) {             /* load/store pair: stp, ldp */
            int rt2 = (insn >> 10) & 31, mode = (insn >> 23) & 3;
            if (insn & (1u << 22)) {
                if (!(insn & (1u << 26))) {                     /* general registers; vector pairs are left alone */
                    if (rt != 31) x[rt] = 0;
                    if (rt2 != 31) x[rt2] = 0;
                }
            }
            if (mode == 1 || mode == 3) {                       /* post- or pre-index: the base still moves */
                int64_t offset = ((int64_t)(int32_t)(insn << 10) >> 25) * ((insn >> 31) ? 8 : 4);
                if (rn == 31) context->uc_mcontext.sp += (uint64_t)offset; else x[rn] += (uint64_t)offset;
            }
            handled = 1;
        } else if ((insn & 0x3B000000u) == 0x38000000u || (insn & 0x3B000000u) == 0x39000000u) {   /* single register */
            int load = (insn >> 22) & 3;                        /* 0 store, others are loads (or prefetch) */
            if (load && !(insn & (1u << 26)) && rt != 31)
                x[rt] = 0;
            if (!(insn & (1u << 24)) && !(insn & (1u << 21)) && (insn & (1u << 10))) {   /* imm9 with writeback */
                int64_t offset = (int64_t)(int32_t)(insn << 11) >> 23;
                if (rn == 31) context->uc_mcontext.sp += (uint64_t)offset; else x[rn] += (uint64_t)offset;
            }
            handled = 1;
        } else if ((insn & 0x3B000000u) == 0x18000000u) {      /* load from a literal */
            if (!(insn & (1u << 26)) && rt != 31) x[rt] = 0;
            handled = 1;
        }
        if (handled) {
            context->uc_mcontext.pc = pc + 4;
            ss_tolerated++;
            return;
        }
    }
    fprintf(stderr, "crash: signal %d at pc %016llx (%s), address %p; %lu stray accesses had been tolerated\n", number,
            (unsigned long long)pc, in_image ? "game image" : "host", info->si_addr, ss_tolerated);
    if (in_image)
        fprintf(stderr, "linked game pc %08X (slide %08X)\n", (unsigned)((uint32_t)pc - image_slide), image_slide);
    _exit(3);
}

#endif

/* A whole file in memory. On Android a path that is not absolute is an asset of the APK. */
static uint8_t *read_whole(const char *path, size_t *size)
{
#ifdef __ANDROID__
    return SDL_LoadFile(path, size);
#else
    FILE *file = fopen(path, "rb");
    uint8_t *data = NULL;
    long length;
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length > 0 && (data = malloc((size_t)length)) != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = data ? (size_t)length : 0;
    return data;
#endif
}

static void release_whole(uint8_t *data)
{
#ifdef __ANDROID__
    SDL_free(data);
#else
    free(data);
#endif
}

#ifndef SS_REBASED_HOST
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
#endif

#ifdef SS_REBASED_HOST
#include "image_format.h"

/* Executable memory is never writable at the same time. Some Android policies
 * reject anonymous execmem: a private memory file supplies the executable pages
 * in that case. MAP_FIXED below only replaces pages this host already owns. */
static int make_executable(void *memory, uint32_t bytes)
{
    int fd = -1, reason;
    size_t written = 0;
    const char *force = getenv("SS_EXEC_MEMFD");
    __builtin___clear_cache((char *)memory, (char *)memory + bytes);
    if ((!force || atoi(force) == 0) && mprotect(memory, bytes, PROT_READ | PROT_EXEC) == 0)
        return 1;
    reason = errno;
#ifdef __NR_memfd_create
    fd = (int)syscall(__NR_memfd_create, "shiningsource-code", 1u);
#endif
    if (fd >= 0) {
        while (written < bytes) {
            ssize_t put = write(fd, (const uint8_t *)memory + written, bytes - written);
            if (put < 0 && errno == EINTR)
                continue;
            if (put <= 0)
                break;
            written += (size_t)put;
        }
        if (written == bytes && mmap(memory, bytes, PROT_READ | PROT_EXEC,
                                     MAP_SHARED | MAP_FIXED, fd, 0) == memory) {
            close(fd);
            __builtin___clear_cache((char *)memory, (char *)memory + bytes);
            fprintf(stderr, "executable code: memory-file RX mapping\n");
            return 1;
        }
        reason = errno;
        close(fd);
    } else
        reason = errno;
    fprintf(stderr, "cannot make code executable: %s (errno %d)\n", strerror(reason), reason);
    return 0;
}

static uint8_t *claim_image(uint32_t linked, uint32_t bytes)
{
    void *got;
    const char *move = getenv("SS_IMAGE_MOVE");
    if (!move || !atoi(move)) {
        if (low_reserved && linked >= LOW_BASE && (uint64_t)linked + bytes <= LOW_END) {
            if (mprotect(PTR(linked), bytes, PROT_READ | PROT_WRITE) == 0)
                return PTR(linked);
            return NULL;
        }
        got = mmap(PTR(linked), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got == PTR(linked))
            return got;
        if (got != MAP_FAILED)
            munmap(got, bytes);
    }
    /* guest_map searches actual gaps and checks the complete 32-bit extent. */
    return guest_map(bytes, PROT_READ | PROT_WRITE);
}

static uint32_t load_image(const uint8_t *data, size_t size)
{
    struct ss_image image;
    char error[192];
    uint8_t *memory;
    uint32_t slide, bytes;
    uint32_t run = 0;
    unsigned previous = 0;
    long page = sysconf(_SC_PAGESIZE);
    unsigned i;
#ifdef __aarch64__
    int macho = 1;
#else
    int macho = 0;
#endif
    if (!ss_image_parse(data, size, macho, &image, error, sizeof error)) {
        fprintf(stderr, "game image rejected: %s\n", error);
        return 0;
    }
    if (page <= 0 || page > 0x10000 || (page & (page - 1))) {
        fprintf(stderr, "unsupported host page size: %ld\n", page);
        return 0;
    }
    bytes = image.high - image.low;
    memory = claim_image(image.low, bytes);
    if (!memory) {
        fprintf(stderr, "no free %u-byte image range below 4 GiB\n", bytes);
        return 0;
    }
    slide = (uint32_t)(uintptr_t)memory - image.low;
    for (i = 0; i < image.count; ++i) {
        const struct ss_image_segment *segment = image.segments + i;
        if (segment->bytes)
            memcpy(memory + segment->address - image.low, data + segment->offset, segment->bytes);
    }
    if (!ss_image_relocate(data, size, &image, memory, slide, error, sizeof error)) {
        fprintf(stderr, "game image relocation rejected: %s\n", error);
        munmap(memory, bytes);
        return 0;
    }
    /* Segment protection boundaries must be real system page boundaries. A
     * combined W+X page is rejected instead of weakening Android's policy. */
    for (uint32_t at = 0; at <= bytes; at += (uint32_t)page) {
        unsigned protection = 0;
        uint64_t linked = (uint64_t)image.low + at;
        for (i = 0; i < image.count; ++i) {
            const struct ss_image_segment *segment = image.segments + i;
            if (at < bytes && segment->memory && linked < (uint64_t)segment->address + segment->memory &&
                linked + (unsigned long)page > segment->address)
                protection |= segment->protection;
        }
        if (at && (protection != previous || at == bytes)) {
            if ((previous & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC) ||
                ((previous & PROT_EXEC) ? !make_executable(memory + run, at - run)
                                       : mprotect(memory + run, at - run, (int)previous) != 0)) {
                fprintf(stderr, "image page permissions failed at %08X (protection %u)\n",
                        (unsigned)(uintptr_t)(memory + run), previous);
                munmap(memory, bytes);
                return 0;
            }
            run = at;
        }
        previous = protection;
        if (at == bytes)
            break;
    }
    image_low = (uint32_t)(uintptr_t)memory;
    image_high = image_low + bytes;
    image_slide = slide;
    fprintf(stderr, "host64: image %s; linked %08X-%08X, actual %08X-%08X, slide %08X\n",
            slide ? "moved" : "at linked address", image.low, image.high, image_low, image_high, slide);
    return image.entry + slide;
}
#else
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

#ifdef __x86_64__
/* ELF32 for x86-64 (the x32 ABI): copy each PT_LOAD segment to its address. Returns the entry point. */
static uint32_t load_image(const uint8_t *data, size_t size)
{
    uint32_t entry, offset, low = ~0u, high = 0;
    int count, stride, i;

    if (size < 52 || memcmp(data, "\x7f" "ELF\x01", 5) != 0)
        return 0;
    entry = rd32(data + 0x18);
    offset = rd32(data + 0x1C);
    stride = data[0x2A] | data[0x2B] << 8;
    count = data[0x2C] | data[0x2D] << 8;
    if ((size_t)offset + (size_t)count * (size_t)stride > size)
        return 0;
    for (i = 0; i < count; i++) {
        const uint8_t *program = data + offset + (uint32_t)(i * stride);
        uint32_t address = rd32(program + 8), memory = rd32(program + 20);
        if (rd32(program) != 1 || !memory) continue;
        if (address < low) low = address;
        if (address + memory > high) high = address + memory;
    }
    low &= ~0xFFFFu;
    high = (high + 0xFFFFu) & ~0xFFFFu;
    if (low >= high || high > image_limit || !low_map(low, high - low, PROT_READ | PROT_WRITE | PROT_EXEC))
        return 0;
    for (i = 0; i < count; i++) {
        const uint8_t *program = data + offset + (uint32_t)(i * stride);
        uint32_t file_offset = rd32(program + 4), address = rd32(program + 8), file_size = rd32(program + 16);
        if (rd32(program) != 1) continue;
        if ((size_t)file_offset + file_size > size) return 0;
        memcpy(PTR(address), data + file_offset, file_size);
    }
    image_low = low;
    image_high = high;
    return entry;
}

#else
/* Mach-O for arm64_32: map each segment at its address and copy its bytes. Returns the entry point. The file is
 * linked without fixups to apply, so loading is all there is to do. */
static uint32_t load_image(const uint8_t *data, size_t size)
{
    uint32_t count, at = 28, entry = 0, text_base = 0, low = ~0u, high = 0, i;

    if (size < 28 || rd32(data) != 0xFEEDFACEu)
        return 0;
    count = rd32(data + 16);
    for (i = 0; i < count && (size_t)at + 8 <= size; i++) {
        const uint8_t *command = data + at;
        uint32_t kind = rd32(command), length = rd32(command + 4);
        if ((size_t)at + length > size || length < 8)
            return 0;
        if (kind == 1 && length >= 56) {                        /* LC_SEGMENT */
            uint32_t address = rd32(command + 24), memory = rd32(command + 28);
            uint32_t offset = rd32(command + 32), bytes = rd32(command + 36), first, last;
            if (address && memory && memcmp(command + 8, "__LINKEDIT", 10) != 0) {
                first = address & ~0xFFFFu;
                last = (address + memory + 0xFFFFu) & ~0xFFFFu;
                if (last > image_limit)
                    return 0;
                if (first < high) first = high;                 /* segments share a 64 KiB block with the previous one */
                if (last > first && !low_map(first, last - first, PROT_READ | PROT_WRITE | PROT_EXEC))
                    return 0;
                if ((size_t)offset + bytes > size)
                    return 0;
                memcpy(PTR(address), data + offset, bytes);
                if (!offset && bytes) text_base = address;
                if ((address & ~0xFFFFu) < low) low = address & ~0xFFFFu;
                if (last > high) high = last;
            }
        } else if (kind == 0x80000028u && length >= 24) {       /* LC_MAIN: offset of the entry */
            entry = rd32(command + 8);
        }
        at += length;
    }
    if (!text_base || !entry)
        return 0;
    image_low = low;
    image_high = high;
    __builtin___clear_cache((char *)PTR(low), (char *)PTR(high));
    return text_base + entry;
}

#endif

#endif /* SS_REBASED_HOST */

/* Run the image's entry on its own low stack. Never returns: the image ends through the exit service. */
uint64_t ss_host_sp __attribute__((visibility("hidden")));
void ss_host_call(void);
#ifdef __x86_64__
void ss_enter_image(uint64_t entry, uint64_t stack_top, uint64_t bridge_table, uint64_t stack_low);
__asm__(
    ".text\n"
    ".globl ss_enter_image\n"
    "ss_enter_image:\n"
    "    movq %rsp, %rax\n"
    "    andq $-16, %rax\n"
    "    subq $256, %rax\n"
    "    movq %rax, ss_host_sp(%rip)\n"    /* where the services run, see ss_host_call */
    "    movq %rsi, %rsp\n"            /* System V in: rdi entry, rsi stack top, rdx bridge, rcx stack low */
    "    movq %rdi, %rax\n"
    "    movl %edx, %edi\n"            /* out: edi bridge, esi stack low, edx stack top */
    "    movl %esi, %edx\n"
    "    movl %ecx, %esi\n"
    "    callq *%rax\n"
    "    ud2\n"
    /* A service called by the image: rax holds its address, the arguments are in place. The image runs on its
     * own stacks in low memory; the system's libraries (and Android's Java runtime, which checks) expect the
     * thread's real stack, so the call is made there. */
    ".globl ss_host_call\n"
    "ss_host_call:\n"
    "    movq %rsp, %r11\n"
    "    movq ss_host_sp(%rip), %rsp\n"
    "    pushq %r11\n"
    "    subq $8, %rsp\n"
    "    callq *%rax\n"
    "    addq $8, %rsp\n"
    "    popq %r11\n"
    "    movq %r11, %rsp\n"
    "    ret\n");

#else
void ss_enter_image(uint64_t entry, uint64_t stack_top, uint64_t bridge_table, uint64_t stack_low);
__asm__(
    ".text\n"
    ".globl ss_enter_image\n"
    ".type ss_enter_image, %function\n"
    "ss_enter_image:\n"
    "    mov x9, sp\n"
    "    sub x9, x9, #256\n"
    "    and x9, x9, #-16\n"
    "    adrp x10, ss_host_sp\n"
    "    str x9, [x10, :lo12:ss_host_sp]\n"    /* where the services run, see ss_host_call */
    "    mov sp, x1\n"                 /* in: x0 entry, x1 stack top, x2 bridge, x3 stack low */
    "    mov x9, x0\n"
    "    mov x10, x1\n"
    "    mov w0, w2\n"                 /* out: w0 bridge, w1 stack low, w2 stack top */
    "    mov w1, w3\n"
    "    mov w2, w10\n"
    "    mov x29, #0\n"
    "    blr x9\n"
    "    brk #1\n"
    /* A service called by the image: x16 holds its address. Same reason as on x86-64: run it on the thread's
     * real stack. */
    ".globl ss_host_call\n"
    ".type ss_host_call, %function\n"
    "ss_host_call:\n"
    "    mov x9, sp\n"
    "    adrp x10, ss_host_sp\n"
    "    ldr x10, [x10, :lo12:ss_host_sp]\n"
    "    mov sp, x10\n"
    "    stp x9, x30, [sp, #-16]!\n"
    "    blr x16\n"
    "    ldp x9, x30, [sp], #16\n"
    "    mov sp, x9\n"
    "    ret\n");

#endif

/* The image calls the services through 32-bit addresses, and this host may be loaded above 4 GiB. Each service
 * gets a few instructions in low memory that load its real address and go through ss_host_call. The table
 * moves there too. */
static uint32_t *low_bridge(const uint64_t *targets, int count)
{
    uint32_t *table;
#ifdef SS_REBASED_HOST
    long page = sysconf(_SC_PAGESIZE);
    uint32_t code_at = page > 0x4000 ? (uint32_t)page : 0x4000;
    uint32_t mapping_bytes = code_at > 0x4000 ? code_at * 2 : 0x10000;
    void *mapping = guest_map(mapping_bytes, PROT_READ | PROT_WRITE);
    uint32_t base;
    if (!mapping)
        return NULL;
    base = (uint32_t)(uintptr_t)mapping;
#else
    uint32_t base = 0x0F800000u;
    uint32_t code_at = 0x1000;
    if (!low_map(base, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC))
        return NULL;
#endif
    uint64_t through = (uint64_t)(uintptr_t)ss_host_call;
    uint8_t *code;
    int i;
    table = PTR(base);
    code = PTR(base + code_at);
    for (i = 0; i < count; i++) {
        uint8_t *slot = code + i * 32;
#ifdef __x86_64__
        static const uint8_t load[2] = { 0x48, 0xB8 };                             /* movabs $target, %rax */
        static const uint8_t jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };     /* jmp *0(%rip) */
        memcpy(slot, load, 2);
        memcpy(slot + 2, &targets[i], 8);
        memcpy(slot + 10, jump, 6);
        memcpy(slot + 16, &through, 8);
#else
        static const uint32_t stub[4] = { 0x58000090u,     /* ldr x16, #16  (the service) */
                                          0x580000B1u,     /* ldr x17, #20  (ss_host_call) */
                                          0xD61F0220u,     /* br x17 */
                                          0xD503201Fu };   /* nop */
        memcpy(slot, stub, 16);
        memcpy(slot + 16, &targets[i], 8);
        memcpy(slot + 24, &through, 8);
#endif
        table[i] = base + code_at + (uint32_t)i * 32;
    }
#ifdef SS_REBASED_HOST
    if (mprotect(PTR(base), code_at, PROT_READ) || !make_executable(PTR(base + code_at), mapping_bytes - code_at)) {
        munmap(PTR(base), mapping_bytes);
        return NULL;
    }
#endif
    __builtin___clear_cache((char *)code, (char *)code + count * 32);
    return table;
}

#ifdef __ANDROID__
static void say(const char *text)
{
    __android_log_print(ANDROID_LOG_INFO, "ShiningSource", "%s", text);
    fprintf(stderr, "%s\n", text);
}

/* KEY=VALUE lines written by the app's first screen (and by tests): the settings of this run. */
static void read_settings(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[0x8000];
    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        char *equals = strchr(line, '='), *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
            *--end = 0;
        if (equals && equals > line) {
            *equals = 0;
            SDL_setenv_unsafe(line, equals + 1, 1);
        }
    }
    fclose(file);
}
#endif

#ifndef __ANDROID__
/* The unmodified AU2P cartridge documented in the README. */
static int verified_rom(const char *path)
{
    enum { ROM_SIZE = 16777216 };
    unsigned char buffer[65536];
    uint32_t crc = 0;
    size_t total = 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    while (total < ROM_SIZE) {
        size_t length = fread(buffer, 1, sizeof buffer, file);
        if (!length)
            break;
        total += length;
        crc = SDL_crc32(crc, buffer, length);
    }
    int valid = total == ROM_SIZE && fgetc(file) == EOF && !ferror(file) && crc == 0x55e503c1;
    fclose(file);
    return valid;
}
#endif

int main(int argc, char **argv)
{
#ifndef SS_REBASED_HOST
    static const struct { uint32_t base, size; const char *name; } regions[] = {
        { SS_EWRAM, 0x40000, "EWRAM" }, { SS_IWRAM, 0x10000, "IWRAM" }, { SS_IO, 0x10000, "I/O registers" },
        { SS_PALETTE, 0x10000, "palette" }, { SS_VRAM, 0x20000, "VRAM" }, { SS_OAM, 0x10000, "OAM" },
        { SS_ROM, 0x1000000, "ROM" },
    };
#endif
    static uint8_t signal_stack[1 << 16];
    struct sigaction action;
    stack_t alternate;
    char image_path[4096];
    const char *rom;
    uint8_t *image;
    size_t image_size = 0;
    uint32_t entry;
    uintptr_t table;
#ifndef SS_REBASED_HOST
    unsigned i;
#endif

#ifdef __ANDROID__
    /* the app's own folder: ROM, saves, settings and the log; the image is an asset */
    const char *folder = SDL_GetAndroidExternalStoragePath();
    static char rom_path[1024];
    (void)argc; (void)argv;
    if (!folder || chdir(folder) != 0) {
        say("startup failure: the app's folder is not available");
        return 2;
    }
    freopen("shiningsource.log", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    read_settings("shiningsource.env");
    read_settings("shiningsource.test.env");        /* tests: SS_FRAMES, SS_INPUT and the like, pushed with adb */
#ifndef SS_HOST_VERSION
#define SS_HOST_VERSION "development"
#endif
    {
        char maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "", api[PROP_VALUE_MAX] = "";
        __system_property_get("ro.product.manufacturer", maker);
        __system_property_get("ro.product.model", model);
        __system_property_get("ro.build.version.release", release);
        __system_property_get("ro.build.version.sdk", api);
        fprintf(stderr, "ShiningSource %s; %s %s; Android %s (API %s); %s; page %ld\n",
                SS_HOST_VERSION, maker, model, release, api,
#ifdef __aarch64__
                "arm64-v8a",
#else
                "x86_64",
#endif
                sysconf(_SC_PAGESIZE));
    }
    snprintf(rom_path, sizeof rom_path, "%s/rom.gba", folder);
    rom = rom_path;
    snprintf(image_path, sizeof image_path, "%s", IMAGE_NAME);
#else
    int prepare = argc > 1 && !strcmp(argv[1], "--prepare");
    int explicit_rom = prepare || (argc > 1 && !strcmp(argv[1], "--game"));
    if (explicit_rom) {
        int flag = prepare ? 2 : 1;
        if (argc != flag + 2 || strcmp(argv[flag], "--game")) {
            fprintf(stderr, "Usage: %s [--prepare] --game ROM.gba\n", argv[0]);
            return 20;
        }
        rom = argv[flag + 1];
        if (prepare) {
            puts("progress 0.1 Checking the European ROM");
            fflush(stdout);
        }
        if (!verified_rom(rom)) {
            fprintf(stderr, "Choose an unmodified Shining Soul II European ROM (AU2P, .gba).\n");
            return 20;
        }
        if (prepare) {
            puts("progress 1.0 European ROM verified");
            return 0;
        }
        restart_args = argv;
    } else {
        rom = argc >= 2 ? argv[1] : getenv("SHININGSOURCE_ROM");
    }
    if (!rom) {
        fprintf(stderr, "usage: %s ROM.gba [game image]   (or set SHININGSOURCE_ROM)\n", argv[0]);
        return 2;
    }
    if (!explicit_rom && argc >= 3) {
        snprintf(image_path, sizeof image_path, "%s", argv[2]);
    } else {
        char *slash;
        ssize_t length = readlink("/proc/self/exe", image_path, sizeof image_path - 32);
        image_path[length > 0 ? length : 0] = 0;
        slash = strrchr(image_path, '/');
        strcpy(slash ? slash + 1 : image_path, IMAGE_NAME);
    }
#endif
#ifdef SS_REBASED_HOST
    /* The guest loads the ROM into its own arena through the bridge. Android
     * may already own any of the original hardware addresses. */
#ifdef __ANDROID__
    SDL_setenv_unsafe("SHININGSOURCE_ROM", rom, 1);
#else
    setenv("SHININGSOURCE_ROM", rom, 1);
#endif
    fprintf(stderr, "host64: rebased GBA memory; page size %ld\n", sysconf(_SC_PAGESIZE));
#else
    for (i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        if (!low_map(regions[i].base, regions[i].size, PROT_READ | PROT_WRITE)) {
            fprintf(stderr, "the address range for the game's %s (0x%08X) is not free; check vm.mmap_min_addr\n",
                    regions[i].name, (unsigned)regions[i].base);
            return 3;
        }
    }
#endif
#ifdef SS_REBASED_HOST
    {
        void *stacks = guest_map(STACK_SLOT * STACK_SLOTS + 0x400000, PROT_READ | PROT_WRITE);
        if (!stacks) {
            fprintf(stderr, "startup failure: no free address range for the game's stacks\n");
            return 3;
        }
        stack_arena = (uint32_t)(uintptr_t)stacks;
        main_stack = stack_arena + STACK_SLOT * STACK_SLOTS;
    }
#else
    if (!low_map(stack_arena, STACK_SLOT * STACK_SLOTS, PROT_READ | PROT_WRITE) || !low_map(main_stack, 0x400000, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "the address range for the game's stacks is not free\n");
        return 3;
    }
#endif
#ifndef SS_REBASED_HOST
    if (load_file(rom, SS_ROM, 0x1000000) < 0x100000) {
        fprintf(stderr, "the ROM file could not be read: %s\n", rom);
        return 2;
    }
#endif
    image = read_whole(image_path, &image_size);
    entry = image ? load_image(image, image_size) : 0;
    if (image)
        release_whole(image);
    if (!entry) {
        fprintf(stderr, "startup failure: the game image could not be loaded: %s\n", image_path);
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
    ENTRY(SS_B_PUMP, b_pump); ENTRY(SS_B_NET_OPEN, b_net_open); ENTRY(SS_B_NET_SEND, b_net_send); ENTRY(SS_B_NET_RECV, b_net_recv);
    ENTRY(SS_B_NET_WAIT, b_net_wait); ENTRY(SS_B_NET_FAREWELL, b_net_farewell); ENTRY(SS_B_TICKS, b_ticks); ENTRY(SS_B_STATUS, b_status); ENTRY(SS_B_OVERLAY, b_overlay);
    ENTRY(SS_B_ACHIEVEMENTS, b_achievements); ENTRY(SS_B_OPTION_HOOK, b_option_hook);
    table = (uintptr_t)low_bridge(bridge, SS_B_COUNT);
    if (!table) {
        fprintf(stderr, "startup failure: the service jumps could not be created\n");
        return 3;
    }
    fprintf(stderr, "host64: image loaded (image %08X-%08X, entry %08X)\n",
            (unsigned)image_low, (unsigned)image_high, (unsigned)entry);
    ss_enter_image(entry, main_stack + 0x400000, table, main_stack);
    return 0;
}
