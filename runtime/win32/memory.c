/* GBA memory map for the dev32 build, where GBA addresses are real addresses.
 *
 * The address ranges cannot be requested from the system at run time: Windows may already have placed
 * something there. They are made part of the executable image instead. The program is linked at a low base
 * and carries one large zero-filled block; the image is the first thing mapped into a new process, so the
 * block is guaranteed, and the GBA regions are simply addresses inside it.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>

char ss_gba_arena[0x08000000];      /* must span 0x02000000-0x09000000; checked below */

int ss_memory_init(const char *rom_path, const char **error)
{
    uintptr_t low = (uintptr_t)ss_gba_arena, high = low + sizeof ss_gba_arena;
    FILE *rom;
    size_t got;

    if (low > SS_EWRAM || high < SS_ROM + 0x1000000) {
        fprintf(stderr, "arena is %08lX-%08lX\n", (unsigned long)low, (unsigned long)high);
        *error = "the executable was not linked so that its arena covers the GBA address space";
        return -1;
    }
    rom = fopen(rom_path, "rb");
    if (!rom) {
        *error = "cannot open the ROM file";
        return -1;
    }
    got = fread((void *)(uintptr_t)SS_ROM, 1, 0x1000000, rom);
    fclose(rom);
    if (got < 0x100000) {
        *error = "ROM file too small";
        return -1;
    }
    return 0;
}
