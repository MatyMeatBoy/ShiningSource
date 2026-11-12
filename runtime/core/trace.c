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

static unsigned long target = ~0ul;
static uint32_t buffer[1 << 20];
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
