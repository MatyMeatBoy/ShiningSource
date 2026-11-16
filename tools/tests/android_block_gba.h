/* Regression fixture: the operating system owns the GBA's hardware addresses.
 * Inject into a headless host with -include, never into a shipping library. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>

__attribute__((constructor)) static void test_block_gba(void)
{
    void *want = (void *)(uintptr_t)0x02000000;
    void *got = mmap(want, 0x08000000, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got != want) {
        fprintf(stderr, "TEST FAILED: cannot occupy original GBA addresses\n");
        exit(99);
    }
    fprintf(stderr, "TEST: original GBA addresses 02000000-0A000000 occupied\n");
}
