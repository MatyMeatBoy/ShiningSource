/* Call trace of one frame, for finding where two builds of the game stop behaving the same.
 *
 * Build the game modules with SHININGSOURCE_TRACE=1 in the environment (tools/build.py adds
 * -finstrument-functions) and run with SS_CALLTRACE=N: every game function entered during frame N is written
 * to ss_calltrace.bin as a 32-bit address. tools/calltrace.py turns two such files into names and shows the
 * first place where they differ.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>

#define NO_TRACE __attribute__((no_instrument_function))

/* Only a call trace build has room for a frame's calls (tools/build.py defines SS_CALL_TRACE with
 * SHININGSOURCE_TRACE): in any other build no function reports here, and 4 MiB that nobody uses would still be
 * charged to every player's process. */
#ifdef SS_CALL_TRACE
enum { TRACE_SLOTS = 1 << 20 };
#else
enum { TRACE_SLOTS = 1 };
#endif
static unsigned long target = ~0ul;
static uint32_t buffer[TRACE_SLOTS];
static unsigned count;
static int written;

NO_TRACE void __cyg_profile_func_enter(void *function, void *call_site)
{
    unsigned long frame;
    (void)call_site;
    if (target == ~0ul) {
        const char *text = getenv("SS_CALLTRACE");
        target = text ? strtoul(text, NULL, 10) : 0;
    }
    if (!target)
        return;
    frame = ss_frame_number();
    if (frame == target && count < sizeof buffer / sizeof buffer[0]) {
        buffer[count++] = (uint32_t)(uintptr_t)function;
    } else if (frame > target && !written) {
        FILE *file = fopen("ss_calltrace.bin", "wb");
        written = 1;
        if (file) {
            fwrite(buffer, 4, count, file);
            fclose(file);
        }
    }
}

NO_TRACE void __cyg_profile_func_exit(void *function, void *call_site)
{
    (void)function;
    (void)call_site;
}

/* Value trace: a temporary source patch (games/<game>/port/patches.json) can call this from a game function to
 * print up to six values during frame SS_VALUES, which shows where two builds compute different numbers when
 * their call traces are the same. */
NO_TRACE void ss_values(const char *tag, int a, int b, int c, int d, int e, int f)
{
    static unsigned long wanted = ~0ul;
    if (wanted == ~0ul) {
        const char *text = getenv("SS_VALUES");
        wanted = text ? strtoul(text, NULL, 10) : 0;
    }
    if (wanted && ss_frame_number() == wanted)
        fprintf(stderr, "values %s %08X %08X %08X %08X %08X %08X\n", tag, (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d,
                (unsigned)e, (unsigned)f);
}
