/* Test of the save file's protection (runtime/core/flash.c): build and run with tools/tests/run_save_test.sh.
 *
 * It drives the real store/load code: a complete write, a write that replaces it (the first becomes the backup),
 * a save cut short, a missing save, and a save folder that cannot be written.
 */
#include <stdint.h>
uint32_t gFlash, ProgramFlashSector, EraseFlashChip, EraseFlashSector;
const uint32_t gSetup512KInfos[1];

#include "../../runtime/core/flash.c"

static int failures;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

int main(void)
{
    static uint8_t first[SECTOR], second[SECTOR];
    FILE *file;
    memset(first, 0x11, SECTOR);
    memset(second, 0x22, SECTOR);
    remove("t.sav"); remove("t.sav.bak"); remove("t.sav.tmp");
    ss_flash_set_path("t.sav");

    load();
    check(memory[0] == 0xFF, "no save: memory reads as an empty cartridge");
    program_sector(0, first);
    program_sector(1, second);
    load();
    check(memory[0] == 0x11 && memory[SECTOR] == 0x22, "two writes are read back");
    file = fopen("t.sav.bak", "rb");
    check(file != NULL, "the save replaced by the last write is kept as backup");
    if (file) fclose(file);
    file = fopen("t.sav.tmp", "rb");
    check(file == NULL, "no temporary file is left behind");
    if (file) fclose(file);

    file = fopen("t.sav", "wb");            /* a save cut short, as after a power loss on a plain write */
    fwrite(first, 1, 1000, file);
    fclose(file);
    load();
    check(memory[0] == 0x11 && memory[SECTOR] == 0xFF, "a save cut short falls back to the previous one, whole");

    remove("t.sav");
    load();
    check(memory[0] == 0x11, "a missing save falls back to the previous one");

    program_sector(1, second);              /* writing again must work from the recovered state */
    load();
    check(memory[0] == 0x11 && memory[SECTOR] == 0x22, "saving after a recovery gives a complete save");

    ss_flash_set_path("no_such_folder/t.sav");
    memset(memory, 0x33, SIZE);
    store();
    ss_flash_set_path("t.sav");
    load();
    check(memory[0] == 0x11 && memory[SECTOR] == 0x22, "a write that cannot be made leaves the existing save alone");

    remove("t.sav"); remove("t.sav.bak"); remove("t.sav.tmp");
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
