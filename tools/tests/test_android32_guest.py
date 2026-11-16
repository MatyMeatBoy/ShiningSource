"""Compile real ARMv7 softfp PIC objects and check their Android shared linkage."""
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_android32


class Android32Guest(unittest.TestCase):
    def test_private_core_stdint_callee_narrows_wide_game_argument(self):
        cc, tools = build_android32.toolchain()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            original = out / 'original'
            (original / 'core').mkdir(parents=True)
            (original / 'include/shiningsource').mkdir(parents=True)
            header = original / 'include/shiningsource/runtime.h'
            header.write_text(r"""
#include <stdint.h>
void song(uint16_t number);
""")
            source = original / 'core/song.c'
            source.write_text(r"""
#include "shiningsource/runtime.h"
void sink(unsigned value);
void song(uint16_t number) { sink(number); }
void control(char key) { sink((unsigned)key); }
""")
            originals = {path: path.read_bytes() for path in (source, header)}
            runtime = build_android32.prepare_runtime_sources(out, original)
            caller = out / 'caller.c'
            caller.write_text(r"""
void song(unsigned number);
volatile unsigned observed;
void sink(unsigned value) { observed = value; }
const char *ss_guest_rom;
void ss_guest_entry(void) { song(0x10032); }
""")
            objects = []
            for source in (runtime / 'core/song.c', caller):
                obj = out / (source.stem + '.o')
                subprocess.run([
                    str(cc), *build_android32.TARGET, '-O2',
                    '-I', str(runtime / 'include'), '-c', str(source),
                    '-o', str(obj),
                ], check=True)
                objects.append(obj)
            guest = build_android32.combine_objects(
                objects, out, tools, wraps_renamed=True,
            )
            shared = out / 'libcore-check.so'
            subprocess.run([
                str(cc), *build_android32.TARGET, '-shared', '-nostdlib',
                '-Wl,--no-undefined', str(guest), '-o', str(shared),
            ], check=True)
            disassembly = subprocess.check_output([
                str(tools / 'llvm-objdump.exe'), '-d', str(shared),
            ], text=True)
            body = re.search(
                r'<song>:(.*?)(?=\n\n|\Z)', disassembly, re.S,
            ).group(1)
            self.assertRegex(body, r'\buxth(?:\.w)?\b')
            for path, contents in originals.items():
                self.assertEqual(path.read_bytes(), contents)

    def test_callee_narrows_negative_wide_alias_argument(self):
        cc, tools = build_android32.toolchain()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / 'src').mkdir()
            callee = out / 'src/glyph.c'
            callee.write_text(r"""
typedef unsigned int u32;
typedef int s32;
void sink(unsigned value);
void glyph(unsigned short y) { sink(y); }
""")
            caller = out / 'src/caller.c'
            caller.write_text(r"""
void wide_alias(int y) __asm__("glyph");
volatile unsigned observed;
void sink(unsigned value) { observed = value; }
const char *ss_guest_rom;
void ss_guest_entry(void) { wide_alias(-48); }
""")
            build_android32.prepare_generated_sources(out)
            objects = []
            for source in (callee, caller):
                obj = source.with_suffix('.o')
                subprocess.run([
                    str(cc), *build_android32.TARGET, '-O2', '-c',
                    str(source), '-o', str(obj),
                ], check=True)
                objects.append(obj)
            guest = build_android32.combine_objects(
                objects, out, tools, wraps_renamed=True,
            )
            shared = out / 'libnarrow-check.so'
            subprocess.run([
                str(cc), *build_android32.TARGET, '-shared', '-nostdlib',
                '-Wl,--no-undefined', str(guest), '-o', str(shared),
            ], check=True)
            disassembly = subprocess.check_output([
                str(tools / 'llvm-objdump.exe'), '-d', str(shared),
            ], text=True)
            body = re.search(
                r'<glyph>:(.*?)(?=\n\n|\Z)', disassembly, re.S,
            ).group(1)
            # AAPCS otherwise trusts the caller to zero-extend u16, passing -48
            # unchanged through a mismatched int declaration. It must be 65488.
            self.assertRegex(body, r'\buxth\b')

    def test_dynamic_ewram_access_survives_guard_optimization(self):
        cc, tools = build_android32.toolchain()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            source = out / 'ewram.c'
            source.write_text(r"""
unsigned read_word(unsigned *pointer) { return *pointer; }
unsigned expect_ewram(void) { return read_word((unsigned *)0x02000000); }
""")
            objects = build_android32.rebased.compile_units(
                str(cc), [(source, ['-O2'], None, 'game')], out,
                build_android32.TARGET, build_android32.STRAY,
                lambda name: None, 1, prefix=False,
            )
            disassembly = subprocess.check_output([
                str(tools / 'llvm-objdump.exe'), '-d', str(objects[0]),
            ], text=True)
            body = re.search(
                r'<expect_ewram>:(.*?)(?=\n\n|\Z)', disassembly, re.S,
            ).group(1)
            # A valid EWRAM read must still load memory after LLVM inlines the
            # dynamic-pointer function; the old broad guard folded it to zero.
            self.assertRegex(body, r'\bldr(?:\.w)?\b')

    def test_same_translation_unit_calls_wrapper_before_optimization(self):
        cc, tools = build_android32.toolchain()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            game = out / 'game.c'
            game.write_text(build_android32.route_wrapped_source(r"""
unsigned sub_08001494(unsigned size);
unsigned sub_08001494(unsigned size) { return 0x1111; }
unsigned same_tu_caller(unsigned size) { return sub_08001494(size); }
void release_alias(void *) __asm__("sub_08001538");
void release_alias(void *pointer) { }
void same_tu_release(void *pointer) { release_alias(pointer); }
const char *ss_guest_rom;
void ss_guest_entry(void) { }
"""))
            wrapper = out / 'wrapper.c'
            wrapper.write_text(r"""
__attribute__((noinline))
unsigned __wrap_sub_08001494(unsigned size) { return size + 0x2222; }
void __wrap_sub_08001538(void *pointer) { }
""")
            objects = []
            for source in (game, wrapper):
                obj = source.with_suffix('.o')
                subprocess.run([
                    str(cc), *build_android32.TARGET, '-O2', '-c',
                    str(source), '-o', str(obj),
                ], check=True)
                objects.append(obj)
            guest = build_android32.combine_objects(
                objects, out, tools, wraps_renamed=True,
            )
            shared = out / 'libwrapper-check.so'
            subprocess.run([
                str(cc), *build_android32.TARGET, '-shared', '-nostdlib',
                '-Wl,--no-undefined', str(guest), '-o', str(shared),
            ], check=True)
            disassembly = subprocess.check_output([
                str(tools / 'llvm-objdump.exe'), '-d', str(shared),
            ], text=True)
            for caller, target in (
                ('same_tu_caller', '__wrap_sub_08001494'),
                ('same_tu_release', '__wrap_sub_08001538'),
            ):
                body = re.search(
                    r'<' + caller + r'>:(.*?)(?=\n\n|\Z)',
                    disassembly, re.S,
                ).group(1)
                self.assertIn('<' + target + '>', body)

    def test_arm_softfp_pic_object_links_without_text_relocations(self):
        cc, tools = build_android32.toolchain()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            source = out / 'fixture.c'
            source.write_text('''
_Static_assert(sizeof(void *) == 4, "32-bit bridge pointers");
const char *ss_guest_rom;
static unsigned value;
unsigned *pointer = &value;
void ss_guest_entry(const unsigned *bridge, unsigned low, unsigned high) {
    *pointer = bridge[0] + low + high;
}
''')
            obj = out / 'fixture.o'
            subprocess.run([
                str(cc), *build_android32.TARGET, '-c', str(source), '-o', str(obj),
            ], check=True)
            coroutine = out / 'co_arm.o'
            subprocess.run([
                str(cc), *build_android32.TARGET, '-c',
                str(build_android32.ROOT / 'runtime/console/co_arm.S'),
                '-o', str(coroutine),
            ], check=True)
            guest = build_android32.combine_objects([obj, coroutine], out, tools)
            shared = out / 'libfixture.so'
            subprocess.run([
                str(cc), *build_android32.TARGET, '-shared', '-nostdlib',
                '-Wl,--no-undefined', str(guest), '-o', str(shared),
            ], check=True)
            headers = subprocess.check_output([
                str(tools / 'llvm-readelf.exe'), '-h', '-A', '-d', str(shared),
            ], text=True)
            self.assertIn('ARM', headers)
            flags = struct.unpack_from('<I', shared.read_bytes(), 36)[0]
            self.assertEqual(flags & 0x600, 0x200)  # EF_ARM_ABI_FLOAT_SOFT
            self.assertIn('VFPv3-D16', headers)
            self.assertNotIn('TEXTREL', headers)
            symbols = subprocess.check_output([
                str(tools / 'llvm-nm.exe'), '--defined-only', '--extern-only', str(guest),
            ], text=True)
            self.assertEqual(
                {line.split()[-1] for line in symbols.splitlines()},
                {'ss_guest_entry', 'ss_guest_rom'},
            )


if __name__ == '__main__':
    unittest.main()
