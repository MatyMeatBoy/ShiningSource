"""Compile the actual portable ROM font reader; fixtures contain no cartridge data.

Run: python tools/tests/test_font_rom.py
The harness includes runtime/win32/font_rom.h and calls ss_font_rom_read directly.
It neither maps GBA addresses nor starts SDL, Android, or the game.
"""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]
ROM_BYTES, ARCHIVE, FONT_BYTES = 0x1000000, 0x518938, 32768
SECTION, FONT = ARCHIVE + 0x80, ARCHIVE + 0xA0
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "font_rom.h"
int main(int argc, char **argv) {
    uint8_t cells[32768];
    int result;
    FILE *out;
    if (argc != 3) return 2;
    result = ss_font_rom_read(argv[1], cells);
    printf("%d\n", result);
    if (result) {
        out = fopen(argv[2], "wb");
        if (!out) return 3;
        if (fwrite(cells, 1, sizeof cells, out) != sizeof cells) return 4;
        if (fclose(out)) return 5;
    }
    return 0;
}
'''


class FontRom(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.tmp.name)
        source = cls.directory / "font_reader.c"
        source.write_text(HARNESS, encoding="utf-8")
        cls.exe = cls.directory / "font_reader.exe"
        subprocess.run([build.clang(), "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime/win32"), str(source),
                        "-o", str(cls.exe)], check=True)
        cls.cells = bytearray((i * 17 + 3) & 255 for i in range(FONT_BYTES))
        cls.cells[0x50 * 128:0x50 * 128 + 64] = bytes(64)
        rom = bytearray(ROM_BYTES)
        rom[0xAC:0xB0] = b"AU2P"
        rom[ARCHIVE:ARCHIVE + 4] = b"SSDT"
        struct.pack_into("<I", rom, ARCHIVE + 24, SECTION - ARCHIVE)
        struct.pack_into("<II", rom, SECTION, 1, FONT - SECTION)
        rom[FONT:FONT + FONT_BYTES] = cls.cells
        cls.valid = bytes(rom)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def read(self, rom):
        path, output = self.directory / "synthetic.gba", self.directory / "cells.bin"
        if output.exists():
            output.unlink()
        if rom is None:
            if path.exists():
                path.unlink()
        else:
            path.write_bytes(rom)
        result = subprocess.run([str(self.exe), str(path), str(output)],
                                check=True, capture_output=True, text=True)
        return int(result.stdout.strip()), output.read_bytes() if output.exists() else None

    def changed_word(self, offset, value):
        rom = bytearray(self.valid)
        struct.pack_into("<I", rom, offset, value)
        return rom

    def test_valid_reader_returns_exact_synthetic_cells(self):
        result, cells = self.read(self.valid)
        self.assertEqual(result, 1)
        self.assertEqual(cells, self.cells)

    def test_wrong_cartridge_code_rejected(self):
        rom = bytearray(self.valid)
        rom[0xAC:0xB0] = b"AU2E"
        self.assertEqual(self.read(rom)[0], 0)

    def test_bad_archive_magic_rejected(self):
        rom = bytearray(self.valid)
        rom[ARCHIVE:ARCHIVE + 4] = b"NOPE"
        self.assertEqual(self.read(rom)[0], 0)

    def test_section_offset_overflow_or_outside_rom_rejected(self):
        for offset in (0xFFF00000, ROM_BYTES, ROM_BYTES - ARCHIVE - 7):
            with self.subTest(offset=offset):
                self.assertEqual(self.read(self.changed_word(ARCHIVE + 24, offset))[0], 0)

    def test_font_offset_overflow_or_end_outside_rom_rejected(self):
        for offset in (0xFFF00000, ROM_BYTES, ROM_BYTES - SECTION - FONT_BYTES + 1):
            with self.subTest(offset=offset):
                self.assertEqual(self.read(self.changed_word(SECTION + 4, offset))[0], 0)

    def test_empty_section_rejected(self):
        self.assertEqual(self.read(self.changed_word(SECTION, 0))[0], 0)

    def test_space_cell_not_empty_rejected(self):
        rom = bytearray(self.valid)
        rom[FONT + 0x50 * 128 + 63] = 1
        self.assertEqual(self.read(rom)[0], 0)

    def test_truncated_header_archive_section_and_font_rejected(self):
        for end in (0xAF, ARCHIVE + 3, SECTION + 7, FONT + FONT_BYTES - 1):
            with self.subTest(end=end):
                self.assertEqual(self.read(self.valid[:end])[0], 0)

    def test_missing_file_rejected(self):
        self.assertEqual(self.read(None)[0], 0)


if __name__ == "__main__":
    unittest.main()
