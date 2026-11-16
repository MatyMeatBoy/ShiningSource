"""Regression: Mach-O C names and explicit assembler aliases name the same game function."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build
import build_arm64
import rebase_ir
import rebased


class MachOSymbols(unittest.TestCase):
    def test_address_taken_core_callback_accepts_cartridge_pointer(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            game, core = out / 'game.c', out / 'core.c'
            game.write_text('void caller(void) {}\n')
            core.write_text('unsigned callback_slot;\n'
                            'static void callback(void *source) { *(volatile unsigned *)source = 7; }\n'
                            'void publish(void) { callback_slot = (unsigned)(__UINTPTR_TYPE__)callback; }\n')
            rebased.compile_units(build.clang(), [(game, [], None, 'game'), (core, [], None, 'core')],
                                  out, build_arm64.GUEST_TARGET, rebase_ir.STRAY_RANGES([(0, 65536)]),
                                  lambda name: None, 1, prefix=True)
            ir = (out / 'ir/core.ll').read_text()
            body = ir.split('define internal void @callback(', 1)[1].split('\n}', 1)[0]
            self.assertIn('%ss.in.', body,
                          'a callback installed in game memory receives cartridge pointers')

    def test_wrapper_translates_argument_to_assembler_aliased_game_function(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            game = out / 'game.c'
            core = out / 'core.c'
            game.write_text('void release(void *p) __asm("___real_sub_08001538");\n'
                            'void release(void *p) { *(volatile unsigned *)p = 1; }\n'
                            'void __wrap_sub_08001538(void *);\n'
                            'void caller(void *p) { __wrap_sub_08001538(p); }\n')
            core.write_text('void __real_sub_08001538(void *);\n'
                            'void __wrap_sub_08001538(void *p) { __real_sub_08001538(p); }\n')
            rebased.compile_units(build.clang(), [(game, [], None, 'game'), (core, [], None, 'core')],
                                  out, build_arm64.GUEST_TARGET, rebase_ir.STRAY_RANGES([(0, 65536)]),
                                  lambda name: None, 1, prefix=True)
            ir = (out / 'ir/core.ll').read_text()
            call = next(line for line in ir.splitlines() if 'call void @__real_sub_08001538(' in line)
            self.assertIn('%ss.out.', call,
                          'free must receive a cartridge pointer, not the arena address')


if __name__ == '__main__':
    unittest.main()
