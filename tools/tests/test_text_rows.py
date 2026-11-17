"""Actual game text-row queuing must not overwrite the adjacent glyph cursor."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]


def text_functions():
    name = 'code_08017F7C.c'
    source = (ROOT / 'games/ss2/decomp/src' / name).read_text()
    patches = json.loads((ROOT / 'games/ss2/port/patches.json').read_text())
    for patch in patches.get(name, []):
        if source.count(patch['find']) != 1:
            raise AssertionError('Patch does not apply once: ' + patch['why'])
        source = source.replace(patch['find'], patch['replace'])
    functions = []
    for declaration in ('static inline void FlushRows16(', 'void sub_0801A3D0(', 's32 sub_0801A664('):
        start = source.index(declaration)
        brace = source.index('{', start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        functions.append(build.ARM_PIN.sub(lambda match: match[1], source[start:end]))
    return '\n'.join(functions)


HARNESS = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#define gUnk_03005C40 ((u8 *)(uintptr_t)0x03005C40)
#define gUnk_03005C54 (*(u16 *)(uintptr_t)0x03005C54)
#define gUnk_03005740 ((u8 *)(uintptr_t)0x03005740)
#define gUnk_03005CA0 ((u32 *)(uintptr_t)0x03005CA0)
struct Copy { const void *source; void *destination; u32 bytes; };
static struct Copy copies[128];
static unsigned count;
static void sub_08001348(const void *source, void *destination, u32 bytes) {
    copies[count++] = (struct Copy){source, destination, bytes};
}
@FUNCTIONS@
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed line %d: %s, cursor=%u\n", __LINE__, #c, gUnk_03005C54); return 1; } } while (0)
static void reset(void) {
    memset((void *)0x03000000, 0, 0x8000);
    memset((void *)0x06000000, 0, 0x18000);
    count = 0;
}
int main(int argc, char **argv) {
    if (!VirtualAlloc((void *)0x03000000, 0x8000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 90;
    if (!VirtualAlloc((void *)0x06000000, 0x18000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 91;
    if (!VirtualAlloc((void *)0x08000000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 92;
    reset();
    if (!strcmp(argv[1], "rows")) {
        gUnk_03005C40[18] = 1;
        sub_0801A664(18, 2);
        CHECK(gUnk_03005C54 == 0);
        CHECK(gUnk_03005C40[19] == 1);
        CHECK(count == 1 && copies[0].destination == (void *)0x0600E4C0 && copies[0].bytes == 64);
        reset();
        gUnk_03005C40[3] = 1;
        sub_0801A664(2, 4);
        CHECK(count == 2);
        CHECK(copies[0].destination == (void *)0x0600E080 && copies[0].bytes == 64);
        CHECK(copies[1].destination == (void *)0x0600E100 && copies[1].bytes == 128);
        CHECK(gUnk_03005C40[6] == 0);
        for (unsigned pattern = 0; pattern < 256; pattern++) {
            unsigned seen[20] = {0};
            reset();
            gUnk_03005C54 = 0xBEEF;
            for (unsigned row = 6; row < 14; row++)
                gUnk_03005C40[row] = (pattern >> (row - 6)) & 1;
            sub_0801A664(6, 8);
            CHECK(gUnk_03005C54 == 0xBEEF);
            for (unsigned copy = 0; copy < count; copy++) {
                uintptr_t destination = (uintptr_t)copies[copy].destination;
                unsigned first = (unsigned)(destination - 0x0600E000) / 64;
                unsigned rows = copies[copy].bytes / 64;
                CHECK(first >= 6 && first + rows <= 14 && rows != 0);
                CHECK(copies[copy].source == gUnk_03005740 + first * 64);
                for (unsigned row = first; row < first + rows; row++) seen[row]++;
            }
            for (unsigned row = 6; row < 14; row++) {
                CHECK(seen[row] == !((pattern >> (row - 6)) & 1));
                CHECK(gUnk_03005C40[row] == 1);
            }
            count = 0;
            sub_0801A664(6, 8);
            CHECK(count == 0 && gUnk_03005C54 == 0xBEEF);
        }
        reset();
        gUnk_03005C54 = 0xBEEF;
        sub_0801A664(0, 0);
        sub_0801A664(20, 2);
        sub_0801A664(0xFFFFFFFFu, 0xFFFFFFFFu);
        CHECK(count == 0 && gUnk_03005C54 == 0xBEEF);
        sub_0801A664(19, 0xFFFFFFFFu);
        CHECK(count == 1 && copies[0].bytes == 64);
        CHECK(copies[0].destination == (void *)0x0600E4C0);
        CHECK(gUnk_03005C54 == 0xBEEF);
    } else {
        u8 script[] = {1, 0xFD};
        u8 *font = (u8 *)0x08000000;
        FILE *file = fopen(argv[2], "rb");
        if (!file || fread(font, 1, 128, file) != 128) return 93;
        fclose(file);
        gUnk_03005CA0[0] = (u32)(uintptr_t)font;
        gUnk_03005C40[18] = gUnk_03005C40[19] = 1;
        sub_0801A3D0(script, 0, 18);
        CHECK(gUnk_03005C54 == 64);
        for (unsigned i = 0; i < count; i++)
            memcpy(copies[i].destination, copies[i].source, copies[i].bytes);
        CHECK(!memcmp((void *)0x0600B000, font, 64));
        CHECK(*(u16 *)0x03005BC0 == 0xF180);
        CHECK(*(u16 *)0x03005C00 == 0xF181);
    }
    return 0;
}
'''


class TextRows(unittest.TestCase):
    def test_reused_bottom_rows_preserve_cursor_and_pending_spans(self):
        self.run_fixture('rows')

    def test_reused_bottom_rows_upload_a_glyph_without_byte_shift(self):
        self.run_fixture('glyph')

    def run_fixture(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            source = out / 'text_rows.c'
            source.write_text(HARNESS.replace('@FUNCTIONS@', text_functions()))
            exe = out / 'text_rows.exe'
            subprocess.run([
                build.clang(), '-std=c11', '-Wno-pointer-to-int-cast',
                '-Wno-int-to-pointer-cast', '-Wno-return-type', str(source), '-o', str(exe),
            ], check=True)
            font = out / 'font.bin'
            # A distinctive 4bpp bitmap detects every byte displacement, without
            # needing or packaging a ROM in this portable regression.
            font.write_bytes(bytes((index * 37 + 11) & 255 for index in range(128)))
            subprocess.run([str(exe), mode, str(font)], check=True)


if __name__ == '__main__':
    unittest.main()
