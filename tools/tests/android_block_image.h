/* Test only: none of the original GBA/image/stack/bridge window is free. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>

static void test_image_preserved(void)
{
    if (*(volatile unsigned char *)(uintptr_t)0x0A000000u != 0xA5) {
        fprintf(stderr, "TEST FAILED: occupied image mapping overwritten\n");
        _Exit(99);
    }
    fprintf(stderr, "TEST: occupied image mapping preserved\n");
}

__attribute__((constructor)) static void test_block_image(void)
{
    void *want = (void *)(uintptr_t)0x02000000u;
    if (mmap(want, 0x0F000000u, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != want) {
        fprintf(stderr, "TEST FAILED: cannot occupy image test range\n");
        exit(99);
    }
    *(volatile unsigned char *)(uintptr_t)0x0A000000u = 0xA5;
    atexit(test_image_preserved);
    fprintf(stderr, "TEST: original GBA/image/stack/bridge window occupied\n");
}
