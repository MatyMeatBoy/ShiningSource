/* GBA memory map for the modes where GBA addresses are real addresses. */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <windows.h>

static int map(uint32_t base, uint32_t size)
{
    return VirtualAlloc((void *)(uintptr_t)base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == (void *)(uintptr_t)base;
}

int ss_memory_init(const char *rom_path, const char **error)
{
    static const struct { uint32_t base, size; const char *name; } regions[] = {
        { SS_EWRAM, 0x40000, "EWRAM" }, { SS_IWRAM, 0x8000, "IWRAM" }, { SS_IO, 0x10000, "I/O registers" },
        { SS_PALETTE, 0x10000, "palette" }, { SS_VRAM, 0x20000, "VRAM" }, { SS_OAM, 0x10000, "OAM" },
        { SS_ROM, 0x1000000, "ROM" },
    };
    unsigned i;
    FILE *rom;
    size_t got;

    for (i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        if (!map(regions[i].base, regions[i].size)) {
            *error = regions[i].name;
            return -1;
        }
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
