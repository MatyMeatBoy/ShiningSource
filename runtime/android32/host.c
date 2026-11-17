/* Native ARMv7 Android host. The localized guest object owns the rebased game,
 * custom libc and coroutines; only ss_guest_entry/ss_guest_rom cross the link.
 * Both sides use Android's ARM softfp ABI. Static bridge stubs switch services
 * back to the real SDL thread stack before entering Android libraries or JNI.
 * No fixed mappings, executable allocations or low-address trampolines are used.
 * SS_ANDROID32_HEADLESS omits SDL and accepts ROM argv[1] for Bionic ARM tests. */
#define _GNU_SOURCE
#include "../guest/bridge.h"
#include "shiningsource/runtime.h"
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    _exit(code);                         /* SDL/Java threads still own process resources */
}
static SERVICE uint32_t b_stack_alloc(uint32_t size)
{
    void *memory = NULL;
    if (!size || posix_memalign(&memory, 16, size) != 0) {
        fprintf(stderr, "coroutines: could not allocate %u bytes\n", size);
        b_exit(1);
    }
    memset(memory, 0, size);
    return (uint32_t)(uintptr_t)memory;
}
static SERVICE void b_stack_free(uint32_t base, uint32_t size) { (void)size; free(PTR(base)); }
static SERVICE void b_stack_bounds(uint32_t low, uint32_t high) { (void)low; (void)high; }
static SERVICE int b_video_init(uint32_t title) { return ss_plat_video_init(PTR(title)); }
static SERVICE uint32_t b_present(uint32_t pixels, int width) { return ss_plat_present(PTR(pixels), width); }
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
static SERVICE void b_achievements(int flags) { (void)flags; }
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
 * softfp double arguments/results also travel in these registers. */
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
STUB(b_achievements) STUB(b_option_hook)
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
    ENTRY(SS_B_OVERLAY, b_overlay); ENTRY(SS_B_ACHIEVEMENTS, b_achievements); ENTRY(SS_B_OPTION_HOOK, b_option_hook);
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
static char *fault_hex(char *out, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    for (shift = 28; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 15];
    return out;
}
static char *fault_text(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    return out;
}
static void on_fault(int number, siginfo_t *info, void *raw)
{
    const ucontext_t *context = raw;
    char message[128], *end = message;
    /* Only fixed local storage, bounded formatting and async-signal-safe write.
     * The alternate stack also reports exhausted guest/coroutine stacks. */
    end = fault_text(end, "crash: signal 0x");
    end = fault_hex(end, (uint32_t)number);
    end = fault_text(end, " at pc 0x");
    end = fault_hex(end, context ? (uint32_t)context->uc_mcontext.arm_pc : 0);
    end = fault_text(end, ", sp 0x");
    end = fault_hex(end, context ? (uint32_t)context->uc_mcontext.arm_sp : 0);
    end = fault_text(end, ", address 0x");
    end = fault_hex(end, info ? (uint32_t)(uintptr_t)info->si_addr : 0);
    *end++ = '\n';
    (void)write(STDERR_FILENO, message, (size_t)(end - message));
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
#ifndef SS_ANDROID32_HEADLESS
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
    rom = argc > 1 ? argv[1] : getenv("SHININGSOURCE_ROM");
#endif
    fprintf(stderr, "ShiningSource %s; armeabi-v7a; pointer %u; page %ld; native rebased guest\n",
            SS_HOST_VERSION, (unsigned)sizeof(void *), sysconf(_SC_PAGESIZE));
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
    install_fault_log();
    setup_bridge();
    ss_guest_rom = rom;
    setenv("SHININGSOURCE_ROM", rom, 1); /* host-side font lookup; no fixed ROM mapping */
    ss32_enter_guest(bridge, stack_base, stack_base + 4 * 1024 * 1024);
}
