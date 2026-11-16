"""Portable image parser and native pointer relocation regression tests."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build


ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
int value = 7;
void game(void) {};
int *data_pointer = &value;
void (*callback)(void) = game;
unsigned gba_literal = 0x08001234;
void entry(void) {}
'''
HARNESS = r'''

#include "runtime/host64/image_format.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  FILE *fp;
  long n;
  unsigned char *f, *m;
  struct ss_image i;
  char e[128];
  unsigned k, hits = 0, literals = 0;
  int ok;
  if (argc != 4)
    return 90;
  fp = fopen(argv[1], "rb");
  if (!fp)
    return 91;
  fseek(fp, 0, SEEK_END);
  n = ftell(fp);
  rewind(fp);
  f = malloc(n);
  fread(f, 1, n, fp);
  fclose(fp);
  ok = ss_image_parse(f, n, atoi(argv[2]), &i, e, sizeof e);
  if (!ok) {
    if (atoi(argv[3]))
      return 0;
    fprintf(stderr, "parse: %s\n", e);
    return 1;
  }
  m = calloc(1, i.high - i.low);
  for (k = 0; k < i.count; k++)
    if (i.segments[k].bytes)
      memcpy(m + i.segments[k].address - i.low, f + i.segments[k].offset,
             i.segments[k].bytes);
  {
    unsigned char *before = malloc(i.high - i.low);
    memcpy(before, m, i.high - i.low);
    ok = ss_image_relocate(f, n, &i, m, 0x100000, e, sizeof e);
    if (atoi(argv[3]))
      return ok ? 2 : 0;
    if (!ok) {
      fprintf(stderr, "relocate: %s\n", e);
      return 3;
    }
    for (k = 0; k + 4 <= i.high - i.low; k += 4) {
      unsigned old = ss_img_u32(before + k), now = ss_img_u32(m + k);
      if (now != old) {
        if (i.format ? now != old + 0x100000
                     : !ss_img_target(&i, now - 0x100000, 1))
          return 4;
        hits++;
      }
      if (old == 0x08001234) {
        if (now != old)
          return 5;
        literals++;
      }
    }
  }
  if (hits < 2 || !literals)
    return 6;
  return 0;
}
'''

class ImageFormat(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.p = Path(cls.tmp.name)
        cls.cc = build.clang()
        cls.bin = Path(cls.cc).parent
        c = cls.p / 'harness.c'
        c.write_text(HARNESS)
        cls.exe = cls.p / 'harness.exe'
        subprocess.run([cls.cc, str(c), '-I', str(ROOT), '-o', str(cls.exe)], check=True)
        source = cls.p / 'game.c'
        source.write_text(SOURCE)
        cls.elf = cls.p / 'game.elf'
        obj = cls.p / 'elf.o'
        subprocess.run(
            [
                cls.cc,
                '--target=x86_64-linux-gnux32',
                '-fPIE',
                '-ffreestanding',
                '-c',
                str(source),
                '-o',
                str(obj),
            ],
            check=True,
        )
        subprocess.run(
            [
                str(cls.bin / 'ld.lld.exe'),
                '-m',
                'elf32_x86_64',
                '-pie',
                '--no-dynamic-linker',
                '-e',
                'entry',
                str(obj),
                '-o',
                str(cls.elf),
            ],
            check=True,
        )
        cls.macho = cls.p / 'game.macho'
        obj = cls.p / 'macho.o'
        subprocess.run(
            [
                cls.cc,
                '--target=arm64_32-apple-watchos6.0',
                '-fPIE',
                '-ffreestanding',
                '-c',
                str(source),
                '-o',
                str(obj),
            ],
            check=True,
        )
        subprocess.run(
            [
                str(cls.bin / 'ld.lld.exe'),
                '-flavor',
                'darwin',
                '-arch',
                'arm64_32',
                '-platform_version',
                'watchos',
                '6.0',
                '6.0',
                '-pie',
                '-no_fixup_chains',
                '-e',
                '_entry',
                str(obj),
                '-o',
                str(cls.macho),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_image(self, data, macho, bad=False):
        p = self.p / 'input.bin'
        p.write_bytes(data)
        subprocess.run([str(self.exe), str(p), str(int(macho)), str(int(bad))], check=True)

    def test_elf_pointers_and_gba_literal(self):
        self.check_image(self.elf.read_bytes(), False)

    def test_macho_pointers_and_gba_literal(self):
        self.check_image(self.macho.read_bytes(), True)

    def test_truncated_images(self):
        for path, macho in [(self.elf, False), (self.macho, True)]:
            for n in [0, 16, 51, 100]:
                self.check_image(path.read_bytes()[:n], macho, True)

    def test_bad_elf_relocation_target(self):
        b = bytearray(self.elf.read_bytes())
        ph = struct.unpack_from('<I', b, 28)[0]
        num = struct.unpack_from('<H', b, 44)[0]
        loads = []
        dyn = None
        for k in range(num):
            t, o, v, _, sz = struct.unpack_from('<IIIII', b, ph + 32 * k)
            if t == 1:
                loads.append((v, o, sz))
            if t == 2:
                dyn = (o, sz)
        ra = None
        for o in range(dyn[0], sum(dyn), 8):
            t, v = struct.unpack_from('<II', b, o)
            if t == 7:
                ra = v
        off = next((o + ra - v for v, o, s in loads if v <= ra < v + s))
        struct.pack_into('<I', b, off, 4294967280)
        self.check_image(b, False, True)

    def test_bad_macho_rebase(self):
        b = bytearray(self.macho.read_bytes())
        o = 28
        for _ in range(struct.unpack_from('<I', b, 16)[0]):
            t, n = struct.unpack_from('<II', b, o)
            if t == 2147483682:
                off = struct.unpack_from('<I', b, o + 8)[0]
                b[off] = 240
                break
            o += n
        self.check_image(b, True, True)

    def test_macho_invalid_rebase_operands(self):
        original = self.macho.read_bytes()
        o = 28
        for _ in range(struct.unpack_from('<I', original, 16)[0]):
            t, n = struct.unpack_from('<II', original, o)
            if t == 2147483682:
                off, size = struct.unpack_from('<II', original, o + 8)
                break
            o += n
        for stream in [
            bytes([0x12, 0]),
            bytes([0x20, 0x80, 0x80, 0x80, 0x80, 0x80]),
            bytes([0x11, 0x2F, 0, 0x51, 0]),
            bytes([0x11, 0x21, 0xFF, 0xFF, 0xFF, 0x7F, 0x51, 0]),
        ]:
            b = bytearray(original)
            b[off:off + len(stream)] = stream
            struct.pack_into('<I', b, o + 12, len(stream))
            self.check_image(b, True, True)
if __name__ == '__main__':
    unittest.main()
