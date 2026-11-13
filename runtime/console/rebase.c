/* GBA memory away from its hardware addresses.
 *
 * A Switch homebrew program cannot have memory below 0x08000000 and the Vita's user memory starts at
 * 0x81000000, so the game's regions cannot be where the cartridge had them. They lie one after another in one
 * block of the program instead: 512 KiB for each of the regions 02..07 (EWRAM, IWRAM, registers, palette, VRAM,
 * OAM), then 16 MiB for the ROM. tools/rebase_ir.py rewrites the game and the runtime core so that a pointer in
 * memory keeps the cartridge's value and is moved into this block when it is read:
 *
 *   ss_delta[top byte of a GBA address]    what to add to find it here (0 for an address of no region)
 *   ss_back[(address - block) / 512 KiB]   what to add to get the cartridge's value back (0 outside the block)
 *
 * The program's own memory must not have a top byte of 02..08, or its addresses would be taken for the GBA's:
 * true on both consoles, and checked by ss_rebase_init for the block itself.
 */
#include "rebase.h"
#include "shiningsource/runtime.h"

#include <stdio.h>

uint8_t ss_arena[SS_ARENA_BYTES] __attribute__((aligned(4096)));
uint32_t ss_delta[256];
uint32_t ss_back[SS_BACK_ENTRIES];
/* An access the console would have ignored (tools/rebase_ir.py): a write goes to ss_scratch, a block read comes
 * from ss_zeros; a single read is given 0 where it is made. */
uint8_t ss_scratch[65536] __attribute__((aligned(16)));
const uint8_t ss_zeros[65536] __attribute__((aligned(16))) = { 0 };

void ss_rebase_init(void)
{
    uint32_t block = (uint32_t)(uintptr_t)ss_arena, region, slot;

    for (region = 2; region < 2 + SS_SLOTS; region++) {
        ss_delta[region] = block + (region - 2) * SS_SLOT - (region << 24);
        ss_back[region - 2] = 0u - ss_delta[region];
    }
    ss_delta[8] = block + SS_ROM_AT - SS_ROM;
    for (slot = SS_SLOTS; slot < SS_BACK_ENTRIES - 1; slot++)
        ss_back[slot] = 0u - ss_delta[8];
    ss_back[SS_BACK_ENTRIES - 1] = 0;
}

void *ss_host(uint32_t gba)
{
    return (void *)(uintptr_t)(gba + ss_delta[gba >> 24]);
}

int ss_memory_init(const char *rom_path, const char **error)
{
    uint32_t block = (uint32_t)(uintptr_t)ss_arena;
    FILE *rom;
    size_t got;

    ss_rebase_init();
    if ((block >> 24) < 9 || ((block + SS_ARENA_BYTES - 1) >> 24) < 9) {
        *error = "the program's memory lies where the GBA's addresses are";
        return -1;
    }
    rom = fopen(rom_path, "rb");
    if (!rom) {
        *error = "cannot open the ROM file";
        return -1;
    }
    got = fread(ss_host(SS_ROM), 1, SS_ROM_BYTES, rom);
    fclose(rom);
    if (got < 0x100000) {
        *error = "ROM file too small";
        return -1;
    }
    return 0;
}

/* Native code is anything that is not a GBA address: a ROM word that was not turned into a native function
 * still has the top byte 08. */
int ss_is_native_code(uint32_t address)
{
    return address >= 0x00010000u && ((address >> 24) < 2 || (address >> 24) > 9);
}

/* Nothing faults here: the rewritten game code tests its own addresses (tools/rebase_ir.py, rule 4) and every
 * division is given the cartridge's result where it is compiled (tools/division_ir.py). */
int ss_stray_access(void *context, uint32_t address, int is_write) { (void)context; (void)address; (void)is_write; return 0; }
int ss_divide_by_zero(void *context) { (void)context; return 0; }
unsigned long ss_stray_count(void) { return 0; }
