/* Native ARMv7 host for Android and for armhf Linux (GA36 handheld, glibc).
 * The localized guest object owns the rebased game,
 * custom libc and coroutines; only ss_guest_entry/ss_guest_rom cross the link.
 * Both sides use one float ABI: Android's softfp, or hard float on armhf Linux
 * (tools/build_ga36.py builds the guest for arm-linux-gnueabihf). The stubs
 * below never touch VFP registers, so d0-d7 arguments/results pass through.
 * Static bridge stubs switch services
 * back to the real SDL thread stack before entering Android libraries or JNI.
 * No fixed mappings, executable allocations or low-address trampolines are used.
 * SS_ANDROID32_HEADLESS omits SDL and accepts ROM argv[1] for Bionic ARM tests.
 * On Linux the ROM is argv[1] or SHININGSOURCE_ROM; shiningsource.env and
 * shiningsource.test.env in the current folder add settings. */
#define _GNU_SOURCE
#include "../guest/bridge.h"
#include "shiningsource/runtime.h"
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>
#include <ucontext.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifndef SS_ANDROID32_HEADLESS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

#ifndef SS_HOST_VERSION
#define SS_HOST_VERSION "development"
#endif
_Static_assert(sizeof(void *) == 4, "Android native32 host requires 32-bit pointers");
#if !defined(__ANDROID__) && !defined(__ARM_PCS_VFP)
#error "armhf Linux host: the guest from tools/build_ga36.py passes doubles in VFP registers (-mfloat-abi=hard)"
#endif
#if defined(__ANDROID__) && defined(__ARM_PCS_VFP)
#error "Android host: the guest from tools/build_android32.py uses softfp"
#endif
void ss_guest_entry(const uint32_t *bridge, uint32_t stack_low, uint32_t stack_high);
extern const char *ss_guest_rom;
#define PTR(value) ((void *)(uintptr_t)(value))
#define SERVICE __attribute__((used))

static FILE *files[32];
static int (*guest_option)(int, int);
static uint32_t bridge[SS_B_COUNT];

static SERVICE uint32_t b_version(void) { return SS_BRIDGE_VERSION; }
static SERVICE int b_write(int handle, uint32_t data, uint32_t size)
{
    FILE *file = handle == 1 ? stdout : handle == 2 ? stderr : handle >= 3 && handle < 32 ? files[handle] : NULL;
    int result = file ? (int)fwrite(PTR(data), 1, size, file) : -1;
    if (file && handle <= 2) fflush(file);
    return result;
}
static SERVICE int b_open(uint32_t path, uint32_t mode)
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
static SERVICE int b_read(int handle, uint32_t data, uint32_t size)
{
    return handle >= 3 && handle < 32 && files[handle] ? (int)fread(PTR(data), 1, size, files[handle]) : -1;
}
static SERVICE int b_close(int handle)
{
    int result = -1;
    if (handle >= 3 && handle < 32 && files[handle]) {
        result = fclose(files[handle]);
        files[handle] = NULL;
    }
    return result;
}
static SERVICE int b_remove(uint32_t path) { return remove(PTR(path)); }
static SERVICE int b_rename(uint32_t from, uint32_t to) { return rename(PTR(from), PTR(to)); }
static SERVICE int b_getenv(uint32_t name, uint32_t out, uint32_t capacity)
{
#ifdef SS_ANDROID32_HEADLESS
    const char *value = getenv(PTR(name));
#else
    const char *value = SDL_getenv(PTR(name));
#endif
    size_t length;
    if (!value)
        value = ss_plat_net_default_env(PTR(name));
    if (!value || !capacity) return -1;
    length = strlen(value);
    if (length >= capacity) length = capacity - 1;
    memcpy(PTR(out), value, length);
    ((char *)PTR(out))[length] = 0;
    return (int)length;
}
static SERVICE void b_exit(int code)
{
    if (code == SS_EXIT_RESTART) code = 0;
    /* Keep the launcher marker identical across Android ABIs. */
    fprintf(stderr, "host64: exit code %d\n", code);
#ifndef SS_ANDROID32_HEADLESS
    {
        extern void ss_plat_last_words(void);
        ss_plat_last_words();
    }
#endif
    fflush(NULL);
    ss_plat_net_send_farewell();
    _exit(code);                         /* SDL/Java threads still own process resources */
}
/* Guest stacks must lie above 0x10000000. The rebased guest reads every address whose top byte is 0x00..0x0F
 * as a GBA address: 0x00/0x01 are stray cartridge accesses (writes dropped, reads zero) and 0x02..0x0E are
 * translated to GBA memory. glibc's heap starts right after a low, non-PIE program (0x01xxxxxx on the GA36),
 * so posix_memalign gave coroutine stacks the guest could not use: the first coroutine keeping a pointer on
 * its stack (the save screen) decompressed into address 0. Anonymous mappings are placed high by the kernel;
 * a low one is refused and asked for again above 0x40000000. */
static SERVICE uint32_t b_stack_alloc(uint32_t size)
{
    size_t length = ((size_t)size + 0xFFF) & ~(size_t)0xFFF;
    void *memory = MAP_FAILED;
    if (size) {
        memory = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory != MAP_FAILED && (uintptr_t)memory < 0x10000000u) {
            munmap(memory, length);
            memory = mmap((void *)(uintptr_t)0x40000000u, length, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory != MAP_FAILED && (uintptr_t)memory < 0x10000000u) {
                munmap(memory, length);
                memory = MAP_FAILED;
            }
        }
    }
    if (memory == MAP_FAILED) {
        fprintf(stderr, "coroutines: could not map %u bytes above 0x10000000\n", size);
        b_exit(1);
    }
    return (uint32_t)(uintptr_t)memory;     /* anonymous mappings are already zero */
}
static SERVICE void b_stack_free(uint32_t base, uint32_t size)
{
    munmap(PTR(base), ((size_t)size + 0xFFF) & ~(size_t)0xFFF);
}
static SERVICE void b_stack_bounds(uint32_t low, uint32_t high) { (void)low; (void)high; }
static SERVICE int b_video_init(uint32_t title) { return ss_plat_video_init(PTR(title)); }
static volatile uint32_t presented, presented_width;
static volatile uintptr_t crash_address;   /* 0: SS_TEST_CRASH writes there */ /* host-side frame count for crash reports (ss_frame_number is the guest's) */
static SERVICE uint32_t b_present(uint32_t pixels, int width)
{
    static long crash_at = -1;          /* SS_TEST_CRASH=N: a write to address 0 at present N (tests the report) */
    if (crash_at < 0)
        crash_at = getenv("SS_TEST_CRASH") ? atol(getenv("SS_TEST_CRASH")) : 0;
    presented++;
    presented_width = (uint32_t)width;
    if (crash_at && presented == (uint32_t)crash_at)
        *(volatile uint32_t *)crash_address = presented;
    {   /* SS_TEST_STRAY=N: at present N read and write where nothing is mapped, as the game's stray accesses do */
        static long stray_at = -1;
        if (stray_at < 0)
            stray_at = getenv("SS_TEST_STRAY") ? atol(getenv("SS_TEST_STRAY")) : 0;
        if (stray_at && presented == (uint32_t)stray_at) {
            volatile uint8_t *far = (volatile uint8_t *)(uintptr_t)(0x211A0066u + crash_address);
            uint8_t seen = *far;
            *far = 7;
            fprintf(stderr, "test: stray read gave %u, write survived%c", (unsigned)seen, 10);
        }
    }
    return ss_plat_present(PTR(pixels), width);
}
static SERVICE int b_view_width(void) { return ss_plat_view_width(); }
static SERVICE void b_audio(uint32_t samples, int frames) { ss_plat_audio(PTR(samples), frames); }
static SERVICE double b_pow(double a, double b) { return pow(a, b); }
static SERVICE double b_sin(double a) { return sin(a); }
static SERVICE double b_cos(double a) { return cos(a); }
static SERVICE double b_fmod(double a, double b) { return fmod(a, b); }
static SERVICE void b_pump(void) { ss_plat_pump(); }
static SERVICE void b_status(uint32_t text) { ss_plat_status(text ? PTR(text) : NULL); }
static SERVICE void b_overlay(int dark, uint32_t text) { ss_plat_overlay(dark, text ? PTR(text) : NULL); }
static SERVICE int b_net_open(uint32_t host, int port) { return ss_plat_net_open(PTR(host), port); }
static SERVICE int b_net_send(uint32_t data, int size) { return ss_plat_net_send(PTR(data), size); }
static SERVICE int b_net_recv(uint32_t data, int capacity) { return ss_plat_net_recv(PTR(data), capacity); }
static SERVICE void b_net_wait(int milliseconds) { ss_plat_net_wait(milliseconds); }
static SERVICE void b_net_farewell(uint32_t data, int size) { ss_plat_net_farewell(data ? PTR(data) : NULL, size); }
static SERVICE uint32_t b_ticks(void) { return ss_plat_ticks_ms(); }
#ifdef SS_ACHIEVEMENTS
/* The achievements (runtime/achievements; the GA36 build): the image keeps the GBA's memory in a block of its own, so
 * on the first frame the host asks it where the two work RAMs are, hashes the ROM file and starts the evaluation.
 * An image that cannot say (an older one answers 0) runs without achievements. */
#include "../achievements/ach_host.h"
static const char *achievements_rom;
static SERVICE void b_achievements(int flags)
{
    static int started;
    if (!started) {
        uint32_t ewram = guest_option ? (uint32_t)guest_option(SS_OPTION_GBA_ADDRESS, (int)SS_EWRAM) : 0;
        uint32_t iwram = guest_option ? (uint32_t)guest_option(SS_OPTION_GBA_ADDRESS, (int)SS_IWRAM) : 0;
        started = 1;
        if (ewram && iwram) {
            ss_ach_set_memory(PTR(ewram), PTR(iwram));
            ss_ach_start_file(achievements_rom);
        } else {
            fprintf(stderr, "achievements: the game image does not say where its memory is; none are evaluated\n");
        }
    }
    ss_ach_frame(flags);
}
#else
static SERVICE void b_achievements(int flags) { (void)flags; }
#endif
/* SS_B_PARALLEL: the image's work(context, index) for every index, on this thread and up to three workers (one
 * per further core; SS_THREADS=N uses N threads in all, 1 = this one alone). An index is taken by whoever is free.
 * The workers wait on a condition between calls; a call returns when every worker has checked in, so the next
 * call never meets a worker still busy with this one. The work never calls back into the host (runtime.h). */
enum { POOL_MAX = 3 };
static pthread_t pool_threads_storage[POOL_MAX];
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_wake = PTHREAD_COND_INITIALIZER, pool_done = PTHREAD_COND_INITIALIZER;
static void (*pool_work)(void *, int);
static void *pool_context;
static int pool_count, pool_size = -1, pool_busy;
static unsigned pool_generation;
static atomic_int pool_next;

static void pool_run(void)
{
    int i;
    while ((i = atomic_fetch_add(&pool_next, 1)) < pool_count)
        pool_work(pool_context, i);
}

static void *pool_main(void *unused)
{
    unsigned seen = 0;
    (void)unused;
    pthread_mutex_lock(&pool_lock);
    for (;;) {
        while (pool_generation == seen)
            pthread_cond_wait(&pool_wake, &pool_lock);
        seen = pool_generation;
        pthread_mutex_unlock(&pool_lock);
        pool_run();
        pthread_mutex_lock(&pool_lock);
        if (--pool_busy == 0)
            pthread_cond_signal(&pool_done);
    }
    return NULL;
}

static void pool_start(void)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    const char *wanted = getenv("SS_THREADS");
    int n = (int)(cores > 1 ? cores - 1 : 0), i;
    if (wanted && *wanted)
        n = atoi(wanted) - 1;
    if (n < 0) n = 0;
    if (n > POOL_MAX) n = POOL_MAX;
    pool_size = 0;
    for (i = 0; i < n; i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&pool_threads_storage[i], &attr, pool_main, NULL) == 0)
            pool_size++;
        pthread_attr_destroy(&attr);
    }
    fprintf(stderr, "host32: %d thread(s) for the picture (%ld core(s))%c", pool_size + 1, cores, 10);
}

/* The same pool for the host's own work (the framebuffer copy, runtime/plat/fbdev_linux.c). Only one caller at a
 * time: the game's thread. */
void ss_host_parallel(void (*work)(void *, int), void *context, int count)
{
    if (pool_size < 0)
        pool_start();
    pool_work = work;
    pool_context = context;
    pool_count = count;
    atomic_store(&pool_next, 0);
    if (pool_size == 0 || count < 2) {
        pool_run();
        return;
    }
    pthread_mutex_lock(&pool_lock);
    pool_busy = pool_size;
    pool_generation++;
    pthread_cond_broadcast(&pool_wake);
    pthread_mutex_unlock(&pool_lock);
    pool_run();
    pthread_mutex_lock(&pool_lock);
    while (pool_busy)
        pthread_cond_wait(&pool_done, &pool_lock);
    pthread_mutex_unlock(&pool_lock);
}

/* SS_B_PARALLEL: the image's function and context are its 32-bit pointers; the calling conventions are the same. */
static SERVICE void b_parallel(uint32_t function, uint32_t context, int count)
{
    ss_host_parallel((void (*)(void *, int))(uintptr_t)function, (void *)(uintptr_t)context, count);
}

static SERVICE void b_option_hook(uint32_t function) { guest_option = (int (*)(int, int))(uintptr_t)function; }
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

/* PC-relative literals keep all stubs position-independent without executable
 * allocations or Android text relocations. All bridge arguments fit r0-r3;
 * softfp double arguments/results also travel in these registers, hard-float
 * ones in d0/d1, which these stubs leave untouched. */
uint32_t ss32_host_sp __attribute__((visibility("hidden")));
void ss32_enter_guest(const uint32_t *table, uint32_t low, uint32_t high) __attribute__((noreturn));
__asm__(
    ".syntax unified\n.thumb\n.text\n.p2align 2\n"
    ".hidden ss32_enter_guest\n.globl ss32_enter_guest\n.type ss32_enter_guest,%function\n.thumb_func\n"
    "ss32_enter_guest:\n"
    "sub.w r3,sp,#256\n"
    "ldr.w r12,1f\n"
    "0: add r12,pc\n"
    "str r3,[r12]\n"
    "mov sp,r2\n"
    "bl ss_guest_entry\n"
    "udf #1\n"
    ".p2align 2\n1: .word ss32_host_sp-(0b+4)\n"
    ".p2align 2\n.hidden ss32_host_call\n.type ss32_host_call,%function\n.thumb_func\n"
    "ss32_host_call:\n"
    "push {r4,lr}\n"
    "mov r4,sp\n"
    "ldr.w lr,1f\n"
    "0: add lr,pc\n"
    "ldr lr,[lr]\n"
    "mov sp,lr\n"
    "push {r4,lr}\n"
    "blx r12\n"
    "pop {r4,lr}\n"
    "mov sp,r4\n"
    "pop {r4,pc}\n"
    ".p2align 2\n1: .word ss32_host_sp-(0b+4)\n");
#define STUB(function) __asm__( ".p2align 2\n.hidden ss32_" #function "\n.globl ss32_" #function \
    "\n.type ss32_" #function ",%function\n.thumb_func\nss32_" #function ":\n" \
    "ldr.w r12,1f\n0: add r12,pc\nb.w ss32_host_call\n.p2align 2\n1: .word " #function "-(0b+4)\n");
STUB(b_version) STUB(b_write) STUB(b_open) STUB(b_read) STUB(b_close) STUB(b_remove) STUB(b_rename)
STUB(b_getenv) STUB(b_exit) STUB(b_video_init) STUB(b_present) STUB(b_audio) STUB(b_stack_alloc)
STUB(b_stack_free) STUB(b_stack_bounds) STUB(b_pow) STUB(b_sin) STUB(b_cos) STUB(b_fmod)
STUB(b_view_width) STUB(b_pump) STUB(b_net_open) STUB(b_net_send) STUB(b_net_recv)
STUB(b_net_wait) STUB(b_net_farewell) STUB(b_ticks) STUB(b_status) STUB(b_overlay)
STUB(b_achievements) STUB(b_option_hook) STUB(b_parallel)
#define ENTRY(index, function) do { extern void ss32_##function(void); \
    bridge[index] = (uint32_t)(uintptr_t)ss32_##function; } while (0)
static void setup_bridge(void)
{
    ENTRY(SS_B_VERSION, b_version); ENTRY(SS_B_WRITE, b_write); ENTRY(SS_B_OPEN, b_open); ENTRY(SS_B_READ, b_read);
    ENTRY(SS_B_CLOSE, b_close); ENTRY(SS_B_REMOVE, b_remove); ENTRY(SS_B_RENAME, b_rename); ENTRY(SS_B_GETENV, b_getenv);
    ENTRY(SS_B_EXIT, b_exit); ENTRY(SS_B_VIDEO_INIT, b_video_init); ENTRY(SS_B_PRESENT, b_present); ENTRY(SS_B_AUDIO, b_audio);
    ENTRY(SS_B_STACK_ALLOC, b_stack_alloc); ENTRY(SS_B_STACK_FREE, b_stack_free); ENTRY(SS_B_STACK_BOUNDS, b_stack_bounds);
    ENTRY(SS_B_POW, b_pow); ENTRY(SS_B_SIN, b_sin); ENTRY(SS_B_COS, b_cos); ENTRY(SS_B_FMOD, b_fmod);
    ENTRY(SS_B_VIEW_WIDTH, b_view_width); ENTRY(SS_B_PUMP, b_pump); ENTRY(SS_B_NET_OPEN, b_net_open);
    ENTRY(SS_B_NET_SEND, b_net_send); ENTRY(SS_B_NET_RECV, b_net_recv); ENTRY(SS_B_NET_WAIT, b_net_wait);
    ENTRY(SS_B_NET_FAREWELL, b_net_farewell); ENTRY(SS_B_TICKS, b_ticks); ENTRY(SS_B_STATUS, b_status);
    ENTRY(SS_B_OVERLAY, b_overlay); ENTRY(SS_B_ACHIEVEMENTS, b_achievements); ENTRY(SS_B_OPTION_HOOK, b_option_hook); ENTRY(SS_B_PARALLEL, b_parallel);
}
#ifndef SS_ANDROID32_HEADLESS
static void read_settings(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[0x8000];
    if (!file) return;
    while (fgets(line, sizeof line, file)) {
        char *equals = strchr(line, '='), *end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
        if (equals && equals > line) {
            *equals = 0;
            SDL_setenv_unsafe(line, equals + 1, 1);
        }
    }
    fclose(file);
}
#endif
/* ---- crash report ----
 * On SIGSEGV/SIGBUS/SIGILL/SIGFPE (on the alternate stack, so an exhausted guest or coroutine stack is reported
 * too): the signal, the fault address, every register of the ucontext, the mapping each code address belongs to
 * ("image+0xOFFSET", OFFSET relative to the image's first mapping: the ELF address of a PIE), the host's present
 * count, and the words of the stack that point into executable mappings. Written to stderr and, on Linux, to
 * shiningsource_crash.log in the current folder (the launcher's user/). tools/crash_names.py --ga36 turns it into
 * function names. Only fixed static storage, bounded formatting and async-signal-safe calls (open/read/write). */
#if !defined(__ANDROID__) && !defined(SS_ANDROID32_HEADLESS)
extern void ss_fb_console_restore(void) __attribute__((weak));   /* runtime/plat/fbdev_linux.c */
#endif
static uint32_t fault_stack_low, fault_stack_high;
static char fault_maps[96 * 1024];
static int fault_maps_size;
static char fault_report[8 * 1024];
static int fault_report_size;
static void fault_put(const char *text)
{
    while (*text && fault_report_size < (int)sizeof fault_report - 1) fault_report[fault_report_size++] = *text++;
}
static void fault_hex(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[11] = "0x";
    int shift, i = 2;
    for (shift = 28; shift >= 0; shift -= 4) text[i++] = digits[(value >> shift) & 15];
    text[i] = 0;
    fault_put(text);
}
static void fault_decimal(uint32_t value)
{
    char text[12], *p = text + sizeof text - 1;
    *p = 0;
    do *--p = (char)('0' + value % 10); while (value /= 10);
    fault_put(p);
}
static uint32_t fault_parse_hex(const char **text)
{
    uint32_t value = 0;
    for (;; (*text)++) {
        char c = **text;
        if (c >= '0' && c <= '9') value = value * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') value = value * 16 + (uint32_t)(c - 'a' + 10);
        else return value;
    }
}
static void fault_read_maps(void)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    fault_maps_size = 0;
    if (fd < 0) return;
    for (;;) {
        ssize_t got = read(fd, fault_maps + fault_maps_size, sizeof fault_maps - 1 - (size_t)fault_maps_size);
        if (got <= 0) break;
        fault_maps_size += (int)got;
    }
    fault_maps[fault_maps_size] = 0;
    close(fd);
}
/* The /proc/self/maps line holding `address`: its range, whether it is executable, and the base of its image
 * (the lowest mapping of the same file). 0 when no line holds it. */
struct fault_map { uint32_t low, high, base; int exec; const char *name; int name_length; };
static int fault_find(uint32_t address, struct fault_map *map)
{
    const char *line = fault_maps;
    while (line < fault_maps + fault_maps_size) {
        const char *p = line, *end = line, *name;
        uint32_t low, high;
        int exec;
        while (*end && *end != '\n') end++;
        low = fault_parse_hex(&p);
        if (*p == '-') p++;
        high = fault_parse_hex(&p);
        exec = p[0] == ' ' && p[3] == 'x';
        name = p;
        {
            int fields = 0;                    /* perms offset dev inode, then the name */
            while (name < end && fields < 4) { while (name < end && *name == ' ') name++; while (name < end && *name != ' ') name++; fields++; }
            while (name < end && *name == ' ') name++;
        }
        if (address >= low && address < high) {
            const char *other = fault_maps;
            map->low = low; map->high = high; map->exec = exec; map->name = name; map->name_length = (int)(end - name);
            map->base = low;
            while (map->name_length && other < fault_maps + fault_maps_size) {
                const char *q = other, *stop = other;
                uint32_t start;
                while (*stop && *stop != '\n') stop++;
                start = fault_parse_hex(&q);
                if (start < map->base && stop - map->name_length >= other
                    && !memcmp(stop - map->name_length, name, (size_t)map->name_length)
                    && (stop - map->name_length == other || stop[-map->name_length - 1] == ' '))
                    map->base = start;
                other = *stop ? stop + 1 : stop;
            }
            return 1;
        }
        line = *end ? end + 1 : end;
    }
    return 0;
}
static void fault_where(uint32_t address)
{
    struct fault_map map;
    if (!fault_find(address, &map)) { fault_put(" (unmapped)"); return; }
    fault_put(" (");
    if (map.name_length) {
        char name[256];
        int length = map.name_length < (int)sizeof name - 1 ? map.name_length : (int)sizeof name - 1;
        memcpy(name, map.name + map.name_length - length, (size_t)length);
        name[length] = 0;
        fault_put(name);
    } else {
        fault_put(address >= fault_stack_low && address < fault_stack_high ? "guest stack" : "anonymous");
    }
    fault_put("+");
    fault_hex(address - (map.name_length ? map.base : map.low));
    fault_put(map.exec ? ", code)" : ")");
}
/* Stray accesses. The game reads and writes through pointers that lead nowhere now and then (an absent player's
 * record, a link queue that only exists with a cable). On the console such an address is open bus: the read gives
 * something meaningless and the write is lost, and the game goes on. The PC builds' fault handlers give the read
 * zero and drop the write. Here a fault of the game's own code on an address nothing is mapped at gets a page of
 * zeros mapped there, and the instruction runs again and reads zero. (What the game writes there stays for its
 * next read of the same place; the PC builds drop it. The game only ever touches these places by mistake.) At
 * most STRAY_PAGES pages: a game that wanders further is reported as a crash. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
enum { STRAY_PAGES = 4096 };
static unsigned stray_pages;
#ifdef __ANDROID__
/* The host is a library there (the game and the runtime are linked into it): its code range is the executable
 * segments of the object holding this file's code, found once before the handler is installed. */
#include <link.h>
static uintptr_t code_low, code_high;
static int find_code(struct dl_phdr_info *object, size_t size, void *unused)
{
    uintptr_t self = (uintptr_t)&find_code, low = UINTPTR_MAX, high = 0;
    int i;
    (void)size; (void)unused;
    for (i = 0; i < object->dlpi_phnum; i++) {
        const ElfW(Phdr) *segment = &object->dlpi_phdr[i];
        if (segment->p_type == PT_LOAD && (segment->p_flags & PF_X)) {
            uintptr_t start = object->dlpi_addr + segment->p_vaddr;
            if (start < low) low = start;
            if (start + segment->p_memsz > high) high = start + segment->p_memsz;
        }
    }
    if (self < low || self >= high)
        return 0;
    code_low = low;
    code_high = high;
    return 1;
}
static void find_code_range(void) { dl_iterate_phdr(find_code, NULL); }
#define CODE_LOW code_low
#define CODE_HIGH code_high
#else
extern const char __executable_start[], etext[];
static void find_code_range(void) {}
#define CODE_LOW ((uintptr_t)__executable_start)
#define CODE_HIGH ((uintptr_t)etext)
#endif
static int stray_page(int number, const siginfo_t *info, const ucontext_t *context)
{
    uintptr_t address, page, pc;
    void *got;
    if (number != SIGSEGV || !info || info->si_code != SEGV_MAPERR || !context || stray_pages >= STRAY_PAGES)
        return 0;
    pc = (uintptr_t)context->uc_mcontext.arm_pc;
    if (pc < CODE_LOW || pc >= CODE_HIGH)
        return 0;                       /* only the program's own code (the game and the runtime) */
    address = (uintptr_t)info->si_addr;
    if (address < 0x10000u)
        return 0;                       /* the kernel keeps the lowest pages unmapped; the guest's guards handle them */
    page = address & ~(uintptr_t)4095;
    got = mmap((void *)page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED)
        return 0;
    if ((uintptr_t)got != page) {       /* an old kernel took the flag for a hint */
        munmap(got, 4096);
        return 0;
    }
    stray_pages++;
    return 1;
}

static void on_fault(int number, siginfo_t *info, void *raw)
{
    if (stray_page(number, info, raw))
        return;                         /* the instruction runs again and finds zeros */
    static const char *const names[] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "fp", "ip" };
    const ucontext_t *context = raw;
    uint32_t regs[13], sp = 0, pc = 0, lr = 0, cpsr = 0;
    int i, fd;
    if (context) {
        const uint32_t *r = (const uint32_t *)&context->uc_mcontext.arm_r0;   /* arm_r0..arm_ip are consecutive */
        for (i = 0; i < 13; i++) regs[i] = r[i];
        sp = (uint32_t)context->uc_mcontext.arm_sp;
        lr = (uint32_t)context->uc_mcontext.arm_lr;
        pc = (uint32_t)context->uc_mcontext.arm_pc;
        cpsr = (uint32_t)context->uc_mcontext.arm_cpsr;
    } else {
        memset(regs, 0, sizeof regs);
    }
    fault_read_maps();
    fault_report_size = 0;
    fault_put("crash: signal ");
    fault_decimal((uint32_t)number);
    fault_put(number == SIGSEGV ? " (SIGSEGV)" : number == SIGBUS ? " (SIGBUS)" : number == SIGILL ? " (SIGILL)" : number == SIGFPE ? " (SIGFPE)" : "");
    fault_put(" code ");
    fault_decimal(info ? (uint32_t)info->si_code : 0);
    fault_put(", address ");
    fault_hex(info ? (uint32_t)(uintptr_t)info->si_addr : 0);
    fault_where(info ? (uint32_t)(uintptr_t)info->si_addr : 0);
    fault_put("\ncrash: host " SS_HOST_VERSION ", thread ");
    fault_decimal((uint32_t)getpid() == (uint32_t)syscall(SYS_gettid) ? 0 : (uint32_t)syscall(SYS_gettid));
    fault_put(" (0: the main thread), present ");
    fault_decimal(presented);
    fault_put(" (width ");
    fault_hex(presented_width);
    fault_put(")\ncrash: pc ");
    fault_hex(pc);
    fault_where(pc);
    fault_put(cpsr & 0x20 ? " thumb" : " arm");
    fault_put("\ncrash: lr ");
    fault_hex(lr);
    fault_where(lr);
    fault_put("\ncrash: sp ");
    fault_hex(sp);
    fault_where(sp);
    fault_put(" cpsr ");
    fault_hex(cpsr);
    for (i = 0; i < 13; i++) {
        fault_put(i % 4 == 0 ? "\ncrash:" : "");
        fault_put(" ");
        fault_put(names[i]);
        fault_put(" ");
        fault_hex(regs[i]);
    }
    fault_put("\n");
    {
        /* Return addresses on the stack: words pointing into executable mappings, up to the stack mapping's end. */
        struct fault_map stack;
        int found = 0;
        if (sp && !(sp & 3) && fault_find(sp, &stack)) {
            uint32_t at, stop = stack.high - sp > 8192 ? sp + 8192 : stack.high;
            for (at = sp; at < stop && found < 48; at += 4) {
                uint32_t word = *(const volatile uint32_t *)(uintptr_t)at;
                struct fault_map code;
                if (word > 0x10000 && fault_find(word, &code) && code.exec) {
                    fault_put("crash: stack sp+");
                    fault_hex(at - sp);
                    fault_put(" ");
                    fault_hex(word);
                    fault_where(word);
                    fault_put("\n");
                    found++;
                }
            }
        }
    }
    {
        /* The executable mappings, for the tool and for addresses in other libraries. */
        const char *line = fault_maps;
        while (line < fault_maps + fault_maps_size) {
            const char *end = line, *p = line;
            while (*end && *end != '\n') end++;
            fault_parse_hex(&p);
            if (*p == '-') { p++; fault_parse_hex(&p); }
            if (p[0] == ' ' && p[3] == 'x' && fault_report_size + (end - line) + 16 < (int)sizeof fault_report) {
                fault_put("crash: map ");
                memcpy(fault_report + fault_report_size, line, (size_t)(end - line));
                fault_report_size += (int)(end - line);
                fault_put("\n");
            }
            line = *end ? end + 1 : end;
        }
    }
    fault_put("crash: end\n");
    (void)write(STDERR_FILENO, fault_report, (size_t)fault_report_size);
#if !defined(__ANDROID__)
    fd = open("shiningsource_crash.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        (void)write(fd, fault_report, (size_t)fault_report_size);
        close(fd);
    }
#else
    (void)fd;
#endif
#if !defined(__ANDROID__) && !defined(SS_ANDROID32_HEADLESS)
    if (ss_fb_console_restore)
        ss_fb_console_restore();       /* text mode and the cursor again, as fbdev's own handler did */
#endif
    _exit(128 + number);
}
static void install_fault_log(void)
{
    static unsigned char alternate_stack[64 * 1024] __attribute__((aligned(16)));
    stack_t stack;
    struct sigaction action;
    memset(&stack, 0, sizeof stack);
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof alternate_stack;
    if (sigaltstack(&stack, NULL) != 0) return;
    find_code_range();
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL); sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL); sigaction(SIGFPE, &action, NULL);
}

int main(int argc, char **argv)
{
    uint32_t stack_base;
    const char *rom;
#if !defined(SS_ANDROID32_HEADLESS) && !defined(__ANDROID__)
    /* armhf Linux: the launcher changes into the game folder first. */
    read_settings("shiningsource.env");
    read_settings("shiningsource.test.env");
    if (argc > 2 && !strcmp(argv[1], "--game")) { argv++; argc--; } /* launchers pass --game ROM */
    rom = argc > 1 ? argv[1] : SDL_getenv("SHININGSOURCE_ROM");
#elif !defined(SS_ANDROID32_HEADLESS)
    static char rom_path[4096];
    const char *folder = SDL_GetAndroidExternalStoragePath();
    (void)argc; (void)argv;
    if (!folder || chdir(folder) != 0) {
        fprintf(stderr, "startup failure: the app's folder is not available\n");
        return 2;
    }
    freopen("shiningsource.log", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    read_settings("shiningsource.env");
    read_settings("shiningsource.test.env");
    snprintf(rom_path, sizeof rom_path, "%s/rom.gba", folder);
    rom = rom_path;
#else
    if (argc > 2 && !strcmp(argv[1], "--game")) { argv++; argc--; } /* launchers pass --game ROM */
    rom = argc > 1 ? argv[1] : getenv("SHININGSOURCE_ROM");
#endif
    fprintf(stderr, "ShiningSource %s; %s; pointer %u; page %ld; native rebased guest\n",
            SS_HOST_VERSION,
#ifdef __ANDROID__
            "armeabi-v7a",
#elif defined(__ARM_PCS_VFP)
            "armhf linux",
#else
            "armel linux",
#endif
            (unsigned)sizeof(void *), sysconf(_SC_PAGESIZE));
#ifdef __ANDROID__
    {
        char maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "", api[PROP_VALUE_MAX] = "";
        __system_property_get("ro.product.manufacturer", maker); __system_property_get("ro.product.model", model);
        __system_property_get("ro.build.version.release", release); __system_property_get("ro.build.version.sdk", api);
        fprintf(stderr, "device: %s %s; Android %s (API %s)\n", maker, model, release, api);
    }
#endif
    if (!rom || !*rom) {
        fprintf(stderr, "startup failure: provide a ROM path\n");
        return 2;
    }
    stack_base = b_stack_alloc(4 * 1024 * 1024);
    fprintf(stderr, "host32: guest stack %08X-%08X; native-thread bridge services\n", stack_base, stack_base + 4 * 1024 * 1024);
    fault_stack_low = stack_base;
    fault_stack_high = stack_base + 4 * 1024 * 1024;
    install_fault_log();
    setup_bridge();
    ss_guest_rom = rom;
#ifdef SS_ACHIEVEMENTS
    achievements_rom = rom;
#endif
    setenv("SHININGSOURCE_ROM", rom, 1); /* host-side font lookup; no fixed ROM mapping */
    ss32_enter_guest(bridge, stack_base, stack_base + 4 * 1024 * 1024);
}
