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

/* Read a whole save file; 1 only if all of it was there. */
static int read_save(const char *path)
{
    FILE *file = fopen(path, "rb");
    size_t got;
    if (!file)
        return 0;
    got = fread(memory, 1, SIZE, file);
    fclose(file);
    return got == SIZE;
}

/* The save, or the previous one (<name>.bak) if the save is missing or cut short: a session that ended in the
 * middle of a write loses at most that write. A cartridge with nothing saved reads as all ones. */
static void load(void)
{
    char backup[600];
    snprintf(backup, sizeof backup, "%s.bak", save_path);
    if (read_save(save_path))
        return;
    if (read_save(backup)) {
        fprintf(stderr, "save: %s was missing or incomplete, the previous save was loaded\n", save_path);
        return;
    }
    memset(memory, 0xFF, SIZE);
}

/* Write the whole memory to a new file and only then put it in place of the save. The save it replaces stays as
 * <name>.bak. If the new file could not be written completely (a full disk), the existing save is not touched. */
static void store(void)
{
    static int told;
    char temporary[600], backup[600];
    FILE *file;
    int complete;
    snprintf(temporary, sizeof temporary, "%s.tmp", save_path);
    snprintf(backup, sizeof backup, "%s.bak", save_path);
    file = fopen(temporary, "wb");
    complete = file && fwrite(memory, 1, SIZE, file) == SIZE;
    if (file && fclose(file) != 0)
        complete = 0;
    if (!complete) {
        if (file)
            remove(temporary);
        if (!told)
            fprintf(stderr, "save: the save file could not be written (%s); the previous save is kept\n", save_path);
        told = 1;
        return;
    }
    remove(backup);
    rename(save_path, backup);  /* fails harmlessly when there is no save yet */
    if (rename(temporary, save_path) != 0 && !told) {
        fprintf(stderr, "save: the new save could not be put in place (%s)\n", save_path);
        told = 1;
    }
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
        if (info[40] == 0xC2 && info[41] == 0x1C) {
            chosen = *entry;
            break;
        }
    }
    /* struct FlashSetupInfo is five pointers (20 bytes) and then struct FlashType, whose maker and device
     * bytes are 20 bytes in. The game compares them with its list of supported chips and refuses to save
     * otherwise, so both offsets have to be exact. */
    gFlash = chosen + 20;
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
