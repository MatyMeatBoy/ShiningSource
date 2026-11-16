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

/* Room identity uses the original cartridge bytes, before native addresses make
 * the mapped ROM depend on the host architecture and image slide. */
static uint32_t rom_fingerprint;
static int rom_fingerprint_valid;

uint32_t ss_rom_fingerprint(void)
{
    if (!rom_fingerprint_valid) {
        const uint8_t *rom = (const uint8_t *)(uintptr_t)SS_ROM;
        uint32_t hash = 2166136261u;
        unsigned offset;
        for (offset = 0; offset < 0x1000000u; offset++)
            hash = (hash ^ rom[offset]) * 16777619u;
        rom_fingerprint = hash;
        rom_fingerprint_valid = 1;
    }
    return rom_fingerprint;
}

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
    (void)ss_rom_fingerprint();
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
