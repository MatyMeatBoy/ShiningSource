/* Pointers to code inside the ROM data.
 *
 * The ROM is mapped where the cartridge had it, so pointers to data are valid as they are. Tables that hold
 * addresses of functions are not: those functions are native now. At load, every word in the scanned ranges
 * whose value is exactly the entry address of a game function is replaced by the native function's address.
 * The table and the ranges are generated per game by tools/link.py.
 */
#include "shiningsource/runtime.h"

struct ss_code_entry { uint32_t gba; void (*native)(void); };
extern const struct ss_code_entry ss_code_table[];
extern const unsigned ss_code_count;
extern const uint32_t ss_code_ranges[][2];
extern const unsigned ss_code_range_count;

static void (*lookup(uint32_t value))(void)
{
    unsigned low = 0, high = ss_code_count;
    while (low < high) {
        unsigned middle = (low + high) / 2;
        if (ss_code_table[middle].gba == value)
            return ss_code_table[middle].native;
        if (ss_code_table[middle].gba < value)
            low = middle + 1;
        else
            high = middle;
    }
    return 0;
}

unsigned ss_patch_code_pointers(void)
{
    unsigned range, patched = 0;
    for (range = 0; range < ss_code_range_count; range++) {
        uint32_t *word = (uint32_t *)(uintptr_t)(SS_ROM + ss_code_ranges[range][0]);
        uint32_t *end = (uint32_t *)(uintptr_t)(SS_ROM + ss_code_ranges[range][1]);
        for (; word < end; word++) {
            void (*native)(void) = (*word >> 24) == 0x08 ? lookup(*word) : 0;
            if (native) {
                *word = (uint32_t)(uintptr_t)native;
                patched++;
            }
        }
    }
    return patched;
}
