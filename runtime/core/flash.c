/* Cartridge save memory (64 KiB flash) backed by a file.
 *
 * The games use the SDK flash library. Its entry points are provided here with the same results; the chip
 * protocol, the routines the library copies to RAM and its timeout timer do not exist in the port.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <string.h>

enum { SECTOR = 0x1000, SIZE = 0x10000 };

/* RAM variables of the library: the game reads the device description and calls through the pointers. */
extern uint32_t gFlash;
extern uint32_t ProgramFlashSector, EraseFlashChip, EraseFlashSector;
/* ROM: zero-terminated list of supported devices (struct FlashSetupInfo *). */
extern const uint32_t gSetup512KInfos[];

static uint8_t memory[SIZE];
static const char *save_path = "shiningsource.sav";

void ss_flash_set_path(const char *path)
{
    save_path = path;
}

static void load(void)
{
    FILE *file = fopen(save_path, "rb");
    memset(memory, 0xFF, SIZE);
    if (file) {
        fread(memory, 1, SIZE, file);
        fclose(file);
    }
}

static void store(void)
{
    char temporary[600];
    FILE *file;
    snprintf(temporary, sizeof temporary, "%s.tmp", save_path);
    file = fopen(temporary, "wb");
    if (!file)
        return;
    fwrite(memory, 1, SIZE, file);
    fclose(file);
    remove(save_path);          /* replace only after the new file is complete */
    rename(temporary, save_path);
}

static uint16_t program_sector(uint16_t sector, const uint8_t *source)
{
    memcpy(memory + (sector & 15) * SECTOR, source, SECTOR);
    store();
    return 0;
}

static uint16_t erase_sector(uint16_t sector)
{
    memset(memory + (sector & 15) * SECTOR, 0xFF, SECTOR);
    store();
    return 0;
}

static uint16_t erase_chip(void)
{
    memset(memory, 0xFF, SIZE);
    store();
    return 0;
}

static void no_timer_interrupt(void)
{
}

uint16_t IdentifyFlash(void)
{
    const uint32_t *entry;
    uint32_t chosen = gSetup512KInfos[0];

    /* The description the game expects is the 512 Kbit Macronix part: maker 0xC2, device 0x1C. */
    for (entry = gSetup512KInfos; *entry >= SS_ROM && *entry < SS_ROM + 0x1000000; entry++) {
        const uint8_t *info = (const uint8_t *)(uintptr_t)*entry;
        if (info[44] == 0xC2 && info[45] == 0x1C) {
            chosen = *entry;
            break;
        }
    }
    gFlash = chosen + 24;       /* struct FlashType inside struct FlashSetupInfo */
    ProgramFlashSector = (uint32_t)(uintptr_t)program_sector;
    EraseFlashSector = (uint32_t)(uintptr_t)erase_sector;
    EraseFlashChip = (uint32_t)(uintptr_t)erase_chip;
    load();
    return 0;
}

uint16_t SetFlashTimerIntr(uint8_t timer, uint32_t *handler_slot)
{
    (void)timer;
    *handler_slot = (uint32_t)(uintptr_t)no_timer_interrupt;
    return 0;
}

void ReadFlash(uint16_t sector, uint32_t offset, void *destination, uint32_t size)
{
    uint32_t start = (sector & 15) * SECTOR + offset;
    if (start + size > SIZE)
        size = start < SIZE ? SIZE - start : 0;
    memcpy(destination, memory + start, size);
}

uint32_t VerifyFlashSector(uint16_t sector, const uint8_t *source)
{
    return memcmp(memory + (sector & 15) * SECTOR, source, SECTOR) ? 1 : 0;
}

uint32_t ProgramFlashSectorAndVerify(uint16_t sector, const uint8_t *source)
{
    program_sector(sector, source);
    return 0;
}
