/* Diagnostic wrapper around the game's allocator (linked with --wrap): the game does not check for a failed
 * allocation, so a NULL result shows up later as a crash somewhere else. Report it where it happens. */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>

void *__real_sub_08001494(uint32_t size);
void __real_sub_08001538(void *block);

static unsigned long live_bytes, live_blocks, peak_bytes;
static int trace = -1;

void *__wrap_sub_08001494(uint32_t size)
{
    void *block = __real_sub_08001494(size);
    if (trace < 0)
        trace = getenv("SS_TRACE_HEAP") != NULL;
    if (block) {
        live_bytes += size;
        live_blocks++;
        if (live_bytes > peak_bytes)
            peak_bytes = live_bytes;
    }
    if (!block || trace) {
        fprintf(stderr, "heap: allocate %u -> %p (live: %lu blocks, %lu bytes requested, peak %lu)\n",
                (unsigned)size, block, live_blocks, live_bytes, peak_bytes);
        fflush(stderr);
    }
    return block;
}

void __wrap_sub_08001538(void *block)
{
    if (block)
        live_blocks--;      /* sizes are not tracked per block; the byte count only grows and is a rough guide */
    __real_sub_08001538(block);
}
