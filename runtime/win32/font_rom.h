#ifndef SHININGSOURCE_FONT_ROM_H
#define SHININGSOURCE_FONT_ROM_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Host presentation must not dereference an original cartridge address when
 * the guest's memory is relocated. Read only the bounded font resource from
 * the user's file; callers fall back to the port's dot font on any failure. */
static inline int ss_font_rom_word(FILE *file, uint32_t offset, uint32_t *value)
{
    uint8_t bytes[4];
    if (fseek(file, (long)offset, SEEK_SET) || fread(bytes, 1, 4, file) != 4)
        return 0;
    *value = bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 1;
}

static inline int ss_font_rom_read(const char *path, uint8_t out[32768])
{
    enum { BYTES = 0x1000000, ARCHIVE = 0x518938, FONT_BYTES = 32768 };
    FILE *file;
    uint8_t magic[4];
    uint32_t relative, section, count, font;
    int ok = 0, i;
    if (!path || !out || !(file = fopen(path, "rb")))
        return 0;
    if (fseek(file, 0xAC, SEEK_SET) || fread(magic, 1, 4, file) != 4 || memcmp(magic, "AU2P", 4))
        goto done;
    if (fseek(file, ARCHIVE, SEEK_SET) || fread(magic, 1, 4, file) != 4 || memcmp(magic, "SSDT", 4))
        goto done;
    if (!ss_font_rom_word(file, ARCHIVE + 24, &relative) || relative > BYTES - ARCHIVE)
        goto done;
    section = ARCHIVE + relative;
    if (section > BYTES - 8 || !ss_font_rom_word(file, section, &count) || !count ||
        !ss_font_rom_word(file, section + 4, &relative) || relative > BYTES - section)
        goto done;
    font = section + relative;
    if (font > BYTES - FONT_BYTES || fseek(file, (long)font, SEEK_SET) ||
        fread(out, 1, FONT_BYTES, file) != FONT_BYTES)
        goto done;
    for (i = 0; i < 64; i++)
        if (out[0x50 * 128 + i])
            goto done;
    ok = 1;
done:
    fclose(file);
    return ok;
}
#endif
