"""ROM room identity must survive native pointer patching and image relocation."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "shiningsource/runtime.h"
struct ss_code_entry { uint32_t gba; void (*native)(void); };
struct ss_code_entry ss_code_table[1];
const unsigned ss_code_count = 1;
const uint32_t ss_code_ranges[][2] = {{0, 8}};
const unsigned ss_code_range_count = 1;
extern unsigned ss_patch_code_pointers(void);
int main(int argc, char **argv) {
    uint32_t *rom, first, second, native;
    if (argc != 4) return 1;
    rom = VirtualAlloc((void *)(uintptr_t)SS_ROM, 0x1000000,
                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (rom != (void *)(uintptr_t)SS_ROM) return 2;
    rom[0] = 0x08000001;
    rom[1] = 0x08001234;
    ((unsigned char *)rom)[8] = (unsigned char)atoi(argv[2]);
    native = strtoul(argv[1], NULL, 0);
    ss_code_table[0].gba = rom[0];
    ss_code_table[0].native = (void (*)(void))(uintptr_t)native;
    if (atoi(argv[3])) first = ss_rom_fingerprint();
    if (ss_patch_code_pointers() != 1) return 3;
    second = ss_rom_fingerprint();
    if (atoi(argv[3]) && first != second) return 4;
    if (rom[0] != native || rom[1] != 0x08001234) return 5;
    if (ss_patch_code_pointers() != 0 || ss_rom_fingerprint() != second) return 6;
    printf("%08x %08x\n", rom[0], second);
    VirtualFree(rom, 0, MEM_RELEASE);
    return 0;
}
'''


# Independent FNV-1a reference values for the 16 MiB fixtures above.
ORIGINAL_FINGERPRINT = 0x9CA7A2C6
VARIANT_FINGERPRINT = 0x991A87A7


class RomIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        directory = Path(cls.tmp.name)
        source = directory / 'rom_identity.c'
        source.write_text(HARNESS)
        cls.exe = directory / 'rom_identity.exe'
        subprocess.run([
            build.clang(), '-Werror=implicit-function-declaration',
            '-I', str(ROOT / 'runtime/include'), str(source),
            str(ROOT / 'runtime/core/codeptr.c'), '-o', str(cls.exe),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_identity(self, native, variant=0, getter_first=False):
        result = subprocess.run([
            str(self.exe), hex(native), str(variant), str(int(getter_first)),
        ], check=True, capture_output=True, text=True)
        return tuple(int(word, 16) for word in result.stdout.split())

    def test_original_rom_identity_survives_different_native_addresses(self):
        first = self.run_identity(0x00100100)
        second = self.run_identity(0x00200200)
        self.assertNotEqual(first[0], second[0])
        expected = ORIGINAL_FINGERPRINT
        self.assertEqual(first[1], expected)
        self.assertEqual(second[1], expected)
        self.assertEqual(self.run_identity(0x00300300, getter_first=True)[1], expected)

    def test_different_raw_rom_has_different_identity(self):
        actual = self.run_identity(0x00100100, variant=1)[1]
        self.assertEqual(actual, VARIANT_FINGERPRINT)
        self.assertNotEqual(actual, self.run_identity(0x00100100)[1])


if __name__ == '__main__':
    unittest.main()
