"""GA36 Ports zip: audit rules, layout and determinism, using a stand-in ARMv7 program."""
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_ga36 as ga36
from launcher_art import SIDE_ART

ROOT = Path(__file__).resolve().parents[2]
SYSROOT = ga36.ARMHF / 'sysroot'
SDL = ga36.ARMHF / 'sdl/lib/libSDL3.so.0.4.18'
SOURCE = r'''
#include <SDL3/SDL.h>
#include <stdio.h>
int main(int argc, char **argv) {
    printf("%s %d\n", SDL_GetPlatform(), argc);
    return 0;
}
'''


def header(machine=40, elf_class=1, flags=0x05000400):
    data = bytearray(52)
    data[:7] = b'\x7fELF' + bytes([elf_class, 1, 1])
    struct.pack_into('<HHI', data, 16, 2, machine, 1)
    struct.pack_into('<I', data, 36, flags)
    return bytes(data)


def toolchain():
    for hint in ga36.READELF_HINTS:
        clang = Path(hint) / 'bin/clang.exe' if hint else None
        if clang and clang.is_file():
            return clang
    return None


def posix_sh():
    for candidate in (shutil.which('sh'), 'C:/Program Files/Git/usr/bin/sh.exe', 'C:/Program Files/Git/bin/sh.exe'):
        if candidate and Path(candidate).is_file():
            return candidate
    return None


class HeaderTest(unittest.TestCase):
    def test_accepts_hard_float_arm(self):
        ga36.check_arm_header('ok', header())

    def test_rejects_other_images(self):
        for data in (header(machine=62), header(elf_class=2), header(flags=0x05000200), b'MZ' + bytes(60), b''):
            with self.assertRaises(ga36.AuditError):
                ga36.check_arm_header('bad', data)

    def test_soft_float_allowed_only_when_asked(self):
        ga36.check_arm_header('diag', header(flags=0x05000200), hard_float=False)


class ScriptTest(unittest.TestCase):
    def test_shell_syntax(self):
        shell = posix_sh()
        if not shell:
            self.skipTest('no POSIX sh')
        for name in (ga36.LAUNCHER, 'ga36-env.sh'):
            subprocess.run([shell, '-n', str(ga36.PORT / name)], check=True)

    def test_launcher_runs_with_fake_loader(self):
        shell = posix_sh()
        if not shell:
            self.skipTest('no POSIX sh')
        with tempfile.TemporaryDirectory() as folder:
            card = Path(folder)
            ports, game = card / 'ports', card / 'ports' / ga36.GAME
            (game / 'libarmhf').mkdir(parents=True)
            (game / 'roms').mkdir()
            fake = card / 'fakebin'
            fake.mkdir()
            (fake / 'uname').write_bytes(b'#!/bin/sh\nif [ "$1" = -m ]; then echo armv7l; else echo Linux; fi\n')
            (game / 'libarmhf/ld-linux-armhf.so.3').write_bytes(
                b'#!/bin/sh\necho "loader $*"\necho "cwd $(pwd)"\nexit 7\n')
            launcher = ports / ga36.LAUNCHER
            launcher.write_bytes(ga36.lf((ga36.PORT / ga36.LAUNCHER).read_bytes()))
            (game / 'ga36-env.sh').write_bytes(ga36.lf((ga36.PORT / 'ga36-env.sh').read_bytes()))
            # Run from the card folder: $PWD has no drive colon to split PATH on under MSYS.
            command = ('export PATH="$PWD/fakebin:$PATH"; chmod +x fakebin/uname '
                       f'"ports/{ga36.GAME}/libarmhf/ld-linux-armhf.so.3"; SS_FBDEV= sh "ports/{ga36.LAUNCHER}" -x')
            missing = subprocess.run([shell, '-c', command], capture_output=True, text=True, cwd=card)
            self.assertEqual(missing.returncode, 5, missing.stderr)
            self.assertIn('no ROM found', missing.stderr)
            self.assertIn('no ROM found', (game / 'user/last-run.log').read_text())
            (card / 'roms/gba').mkdir(parents=True)
            (card / 'roms/gba/Shining Soul II (Europe).gba').write_bytes(b'stand-in')
            found = subprocess.run([shell, '-c', command], capture_output=True, text=True, cwd=card)
            self.assertEqual(found.returncode, 7, found.stderr)
            log = (game / 'user/last-run.log').read_text()
            self.assertIn('--library-path', log)
            self.assertRegex(log, r'ss2_ga36 --game \S*roms/gba/Shining Soul II \(Europe\)\.gba -x')
            self.assertRegex(log, r'cwd \S*/user')
            self.assertIn('exit status: 7', log)
            self.assertIn('no ROM found', (game / 'user/last-run.1.log').read_text())
            self.assertEqual(sorted(p.name for p in (card / 'roms/gba').iterdir()), ['Shining Soul II (Europe).gba'])

    def test_scripts_use_the_explicit_loader(self):
        env = (ga36.PORT / 'ga36-env.sh').read_text(encoding='utf-8')
        self.assertIn('"$game/libarmhf/ld-linux-armhf.so.3" --library-path "$game/libarmhf"', env)
        self.assertIn('"$game/ss2_ga36" --game "$rom" "$@"', env)
        self.assertNotIn('cp ', env)


@unittest.skipUnless(toolchain() and SYSROOT.is_dir() and SDL.is_file(), 'needs llvm-mingw and the armhf sysroot')
class PackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = Path(tempfile.mkdtemp(prefix='ss2-ga36-test-'))
        clang = toolchain()
        cls.readelf = str(clang.with_name('llvm-readelf.exe'))
        source = cls.folder / 'standin.c'
        source.write_text(SOURCE, encoding='ascii')
        gcc = sorted((SYSROOT / 'usr/lib/gcc/arm-linux-gnueabihf').iterdir())[-1]
        cls.binary = cls.folder / 'ss2_ga36'
        subprocess.run([
            str(clang), '--target=arm-linux-gnueabihf', f'--sysroot={SYSROOT.as_posix()}',
            f'--gcc-install-dir={gcc.as_posix()}', '-mcpu=cortex-a7', '-mfloat-abi=hard', '-fuse-ld=lld', '-O2',
            '-I', str(ga36.ARMHF / 'sdl/include'), str(source), str(SDL),
            '-Wl,--dynamic-linker=libarmhf/ld-linux-armhf.so.3', '-Wl,-rpath,$ORIGIN/libarmhf',
            '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def package(self, output, *extra):
        return ga36.main(['--binary', str(self.binary), '--readelf', self.readelf, '--commit', 'test',
                          '--output', str(output), *extra])

    def test_layout_modes_and_manifest(self):
        output = self.folder / 'normal/pkg.zip'
        self.package(output)
        with zipfile.ZipFile(output) as archive:
            names = archive.namelist()
            self.assertEqual(names, sorted(names))
            lib = f'{ga36.GAME}/libarmhf/'
            for name in ('ld-linux-armhf.so.3', 'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
                         'librt.so.1', 'libSDL3.so.0', 'libasound.so.2', 'libasound.so'):
                self.assertIn(lib + name, names)
            for name in ('ss2_ga36', 'ga36-env.sh', 'README.md', 'LICENSE.txt', 'SDL3-LICENSE.txt',
                         'roms/README.txt', 'GA36-release.json', 'alsa/alsa.conf',
                         *(f'assets/launcher/{art}' for art in SIDE_ART)):
                self.assertIn(f'{ga36.GAME}/{name}', names)
            self.assertIn(ga36.LAUNCHER, names)
            self.assertFalse(any(n.startswith(f'{ga36.GAME}/diagnostic/') for n in names))
            self.assertFalse(any(n.lower().endswith('.gba') for n in names))
            for info in archive.infolist():
                self.assertEqual(info.create_system, 3)
                mode = info.external_attr >> 16 & 0o777
                executable = info.filename.endswith('.sh') or info.filename.endswith('/ss2_ga36') \
                    or info.filename.endswith('ld-linux-armhf.so.3')
                if executable:
                    self.assertEqual(mode, 0o755, info.filename)
                if info.filename.endswith(('.sh', '.md', '.xml')):
                    self.assertNotIn(b'\r\n', archive.read(info))
            manifest = json.loads(archive.read(f'{ga36.GAME}/GA36-release.json'))
            self.assertEqual(manifest['version'], ga36.VERSION)
            self.assertEqual(manifest['commit'], 'test')
            for name, digest in manifest['files'].items():
                self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)
            self.assertEqual(set(manifest['files']), set(names) - {f'{ga36.GAME}/GA36-release.json'})
        digest = hashlib.sha256(output.read_bytes()).hexdigest()
        self.assertEqual(output.with_name('pkg.zip.sha256').read_text(), f'{digest}  pkg.zip\n')
        again = self.folder / 'normal/again.zip'
        self.package(again)
        self.assertEqual(again.read_bytes(), output.read_bytes())

    def test_diagnostic_and_extra_binary(self):
        diag = self.folder / 'diag'
        (diag / 'tools').mkdir(parents=True)
        (diag / 'ShiningSource GA36 Diagnostic.sh').write_bytes(b'#!/bin/sh\r\necho ok\r\n')
        (diag / 'tools/probe.sh').write_bytes(b'#!/bin/sh\n')
        (diag / 'notes.txt').write_text('notes', encoding='ascii')
        extra = self.folder / 'ss2_ga36_headless'
        shutil.copyfile(self.binary, extra)
        output = self.folder / 'diag-out/pkg.zip'
        self.package(output, '--diagnostic', str(diag), '--extra-binary', str(extra))
        with zipfile.ZipFile(output) as archive:
            names = set(archive.namelist())
            self.assertIn('ShiningSource GA36 Diagnostic.sh', names)
            self.assertEqual(archive.read('ShiningSource GA36 Diagnostic.sh'), b'#!/bin/sh\necho ok\n')
            self.assertIn(f'{ga36.GAME}/diagnostic/tools/probe.sh', names)
            self.assertIn(f'{ga36.GAME}/diagnostic/notes.txt', names)
            self.assertIn(f'{ga36.GAME}/ss2_ga36_headless', names)
            self.assertTrue(json.loads(archive.read(f'{ga36.GAME}/GA36-release.json'))['diagnostic'])

    def test_rejects_wrong_binaries(self):
        wrong_interp = self.folder / 'wrong_interp'
        subprocess.run([str(toolchain()), '--target=arm-linux-gnueabihf', f'--sysroot={SYSROOT.as_posix()}',
                        f'--gcc-install-dir={sorted((SYSROOT / "usr/lib/gcc/arm-linux-gnueabihf").iterdir())[-1].as_posix()}',
                        '-mfloat-abi=hard', '-fuse-ld=lld', str(self.folder / 'standin.c'), '-I',
                        str(ga36.ARMHF / 'sdl/include'), str(SDL), '-o', str(wrong_interp)], check=True)
        windows = Path(sys.executable)
        for index, binary in enumerate((wrong_interp, windows, ga36.PORT / 'README.md')):
            output = self.folder / f'wrong/{index}.zip'
            with self.assertRaises(SystemExit) as caught:
                ga36.main(['--binary', str(binary), '--readelf', self.readelf, '--output', str(output)])
            self.assertEqual(caught.exception.code, 1)
            self.assertFalse(output.exists())

    def test_rejects_newer_glibc_requirement(self):
        real = ga36.glibc_versions

        def newer(readelf, path):
            return {(2, 99)} if Path(path).name == 'ss2_ga36' else real(readelf, path)
        output = self.folder / 'newer/pkg.zip'
        ga36.glibc_versions = newer
        try:
            with self.assertRaises(SystemExit):
                self.package(output)
        finally:
            ga36.glibc_versions = real
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
