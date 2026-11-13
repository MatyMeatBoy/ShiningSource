/* The port in a browser (WebAssembly, Emscripten): start-up, memory, coroutines.
 *
 * Memory: a WebAssembly program has one block of memory that starts at address 0, so the GBA's addresses are
 * simply used as they are. The program's own data is linked above them (tools/build_web.py), and the game's
 * variables at fixed addresses are constants in the code (tools/absolute_ir.py): there is nothing to map.
 * An access the console would have ignored lands in unused memory instead of faulting.
 *
 * Coroutines: the game's tasks run on Emscripten's fibers (Asyncify).
 *
 * The ROM is the user's: the page writes the chosen file to /rom.gba and this program waits for it.
 * No online play here: a browser has no UDP sockets, so the link driver stays the cartridge's own.
 */
#include "shiningsource/runtime.h"

#include <emscripten.h>
#include <emscripten/fiber.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void AgbMain(void);

/* ---- memory ---- */

int ss_memory_init(const char *rom_path, const char **error)
{
    FILE *file = fopen(rom_path, "rb");
    size_t got;
    if (!file) {
        *error = "the ROM file could not be opened";
        return -1;
    }
    got = fread((void *)(uintptr_t)SS_ROM, 1, 0x1000000, file);
    fclose(file);
    if (got < 0x100000) {
        *error = "the ROM file is too short";
        return -1;
    }
    return 0;
}

/* Function pointers are indices into the function table here: small numbers, never GBA addresses. */
int ss_is_native_code(uint32_t address)
{
    return address != 0 && address < 0x00100000u;
}

int ss_stray_access(void *context, uint32_t address, int is_write) { (void)context; (void)address; (void)is_write; return 0; }
int ss_divide_by_zero(void *context) { (void)context; return 0; }
unsigned long ss_stray_count(void) { return 0; }

/* ---- coroutines ---- */

enum { C_STACK = 256 * 1024, ASYNC_STACK = 64 * 1024 };
struct Fiber { emscripten_fiber_t fiber; void (*entry)(void *); void *argument; };
static emscripten_fiber_t main_fiber;
static emscripten_fiber_t *running = &main_fiber;
static char main_async_stack[ASYNC_STACK];

void ss_co_init(void)
{
    emscripten_fiber_init_from_current_context(&main_fiber, main_async_stack, sizeof main_async_stack);
}

static void fiber_entry(void *parameter)
{
    struct Fiber *self = parameter;
    self->entry(self->argument);
}

void *ss_co_create(void (*entry)(void *), void *argument)
{
    struct Fiber *co = malloc(sizeof *co);
    char *stacks = malloc(C_STACK + ASYNC_STACK);
    co->entry = entry;
    co->argument = argument;
    emscripten_fiber_init(&co->fiber, fiber_entry, co, stacks, C_STACK, stacks + C_STACK, ASYNC_STACK);
    return co;
}

void ss_co_switch(void *coroutine)
{
    emscripten_fiber_t *from = running, *to = coroutine ? &((struct Fiber *)coroutine)->fiber : &main_fiber;
    if (from == to)
        return;
    running = to;
    emscripten_fiber_swap(from, to);
}

void ss_co_delete(void *coroutine)
{
    struct Fiber *co = coroutine;
    if (&co->fiber == running)
        return;                         /* a coroutine cannot free the stack it runs on */
    free(co->fiber.stack_limit);
    free(co);
}

/* ---- the cartridge's division helpers ----
 * The game's own divisions are compiled inline (tools/division_ir.py gives them the cartridge's results); these
 * names remain in the table of functions the ROM's data can point to. Same contract: zero for a zero divisor,
 * the dividend for the most negative number divided by -1. */
int __divsi3(int a, int b) { return b == 0 ? 0 : (b == -1 ? (int)(0u - (unsigned)a) : a / b); }
int __modsi3(int a, int b) { return b == 0 || b == -1 ? 0 : a % b; }
unsigned __udivsi3(unsigned a, unsigned b) { return b == 0 ? 0 : a / b; }
unsigned __umodsi3(unsigned a, unsigned b) { return b == 0 ? 0 : a % b; }

/* ---- no network in a browser ---- */

int ss_net_active(void) { return 0; }
void ss_net_vote(int wants) { (void)wants; }
void ss_net_votes(int *have, int *need) { *have = 0; *need = 1; }
int ss_net_skip_now(void) { return 0; }
unsigned long ss_net_label(void) { return 0; }
void ss_net_leave(void) {}
void ss_net_flush(void) {}
int ss_net_msg_online(void) { return 0; }
int ss_net_msg_send(const void *data, int size) { (void)data; (void)size; return 0; }
int ss_net_msg_recv(int who, void *data, int capacity) { (void)who; (void)data; (void)capacity; return 0; }
int ss_net_msg_next(int *who, void *data, int capacity) { (void)who; (void)data; (void)capacity; return 0; }
int ss_net_msg_gone(int who) { (void)who; return 0; }
void ss_plat_net_wait(int milliseconds) { emscripten_sleep((unsigned)milliseconds); }
uint32_t ss_plat_ticks_ms(void) { return (uint32_t)emscripten_get_now(); }

/* ---- saves ----
 * The game's save file (runtime/core/flash.c: shiningsource.sav and its .tmp/.bak, in the current directory)
 * lives in /save, a folder the browser keeps between visits (IndexedDB). It is read before the game starts.
 * The page then looks every few seconds whether the game wrote, and once more when the page is hidden or left,
 * and stores the folder again if it did. */
EM_ASYNC_JS(int, web_saves_open, (void), {
    try { FS.mkdir('/save'); } catch (e) {}
    FS.mount(IDBFS, {}, '/save');
    var failed = await new Promise(function (resolve) { FS.syncfs(true, function (error) { resolve(error ? 1 : 0); }); });
    var stored = -1, busy = false;
    function written() {                /* when the game last put a save in place; 0 before the first one */
        try { return FS.stat('/save/shiningsource.sav').mtime.getTime(); } catch (e) { return 0; }
    }
    function keep() {
        var now = written();
        if (busy || now === stored) return;
        busy = true;
        FS.syncfs(false, function (error) {
            busy = false;
            if (error) console.log('save: the browser did not store the save (' + error + ')');
            else stored = now;
        });
    }
    stored = written();
    setInterval(keep, 3000);
    addEventListener('pagehide', keep);
    document.addEventListener('visibilitychange', function () { if (document.hidden) keep(); });
    return failed;
});

/* ---- start ---- */

int main(int argc, char **argv)
{
    const char *error = "";
    FILE *probe;
    (void)argc; (void)argv;
    /* Showing a frame does not wait for the display in a browser (it only hands control back for a moment), so
     * the display cannot pace the game here: the game keeps the console's own 59.7275 Hz. */
    setenv("SS_VSYNC", "0", 0);
    if (web_saves_open() != 0)
        fprintf(stderr, "save: the browser's storage could not be read; saves will not outlive this visit\n");
    /* the page puts the user's file there when they choose it */
    while ((probe = fopen("/rom.gba", "rb")) == NULL)
        emscripten_sleep(100);
    fclose(probe);
    if (ss_memory_init("/rom.gba", &error) != 0) {
        fprintf(stderr, "%s\n", error);
        return 2;
    }
    fprintf(stderr, "%u code pointers in the ROM data now point to native functions\n", ss_patch_code_pointers());
    chdir("/save");
    AgbMain();
    return 0;
}
