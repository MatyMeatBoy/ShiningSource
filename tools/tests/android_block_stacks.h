/* Inject only into the headless regression host. Android owns these ranges. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>

static const uintptr_t test_stack_ranges[] = { 0x0C800000u, 0x20000000u };
static const size_t test_stack_sizes[] = { 0x04800000u, 0x05000000u };

static void test_stacks_preserved(void)
{
    unsigned i;
    for (i = 0; i < 2; ++i)
        if (*(volatile unsigned char *)test_stack_ranges[i] != 0xA5) {
            fprintf(stderr, "TEST FAILED: occupied mapping overwritten\n");
            _Exit(99);
        }
    fprintf(stderr, "TEST: occupied stack mappings preserved\n");
}

__attribute__((constructor)) static void test_block_stacks(void)
{
    unsigned i;
    if (getenv("SS_TEST_EXHAUST_STACKS") || getenv("SS_TEST_STACK_HOLE")) {
        void *want = (void *)(uintptr_t)0x11000000u;
        if (mmap(want, 0xEF000000u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != want) {
            fprintf(stderr, "TEST FAILED: cannot occupy all 32-bit stack space\n");
            exit(99);
        }
        if (getenv("SS_TEST_STACK_HOLE")) {
            if (munmap((void *)(uintptr_t)0x15000000u, 0x04400000u) ||
                munmap((void *)(uintptr_t)0x1A000000u, 0x10000u))
                exit(99);
            fprintf(stderr, "TEST: only stack/bridge holes below first hint available\n");
        } else
            fprintf(stderr, "TEST: all 32-bit stack space occupied\n");
        return;
    }
    for (i = 0; i < 2; ++i) {
        void *want = (void *)test_stack_ranges[i];
        void *got = mmap(want, test_stack_sizes[i], PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got != want) {
            fprintf(stderr, "TEST FAILED: cannot occupy stack test range\n");
            exit(99);
        }
        *(volatile unsigned char *)got = 0xA5;
    }
    atexit(test_stacks_preserved);
    fprintf(stderr, "TEST: old stacks/bridge and first allocation hint occupied\n");
}
