#!/usr/bin/env python3
"""Build a manual Ports package using a Focal AArch64 SDK and a released Android game image."""

import argparse
import hashlib
import re
import stat
import subprocess
import zipfile
from pathlib import Path

import build
from launcher_art import SIDE_ART

ROOT = build.ROOT
PORT = ROOT / 'platform/r36s'
STAGE = ROOT / 'build/r36s'


def elf_report(readelf, path, executable):
    output = subprocess.check_output([readelf, '-h', '-l', '-d', '-V', str(path)], text=True)
    if not re.search(r'Machine:\s+AArch64', output) or not re.search(r'Class:\s+ELF64', output):
        raise ValueError(f'Expected AArch64 ELF64 in {path}')
    if executable and 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' not in output:
        raise ValueError(f'Unexpected interpreter in {path}')
    requirements = [tuple(map(int, v.split('.'))) for v in re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)', output)]
    if max(requirements, default=(0,)) > (2, 31) or 'GLIBC_PRIVATE' in output:
        raise ValueError(f'Incompatible with Ubuntu Focal: {path}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--sysroot', required=True, type=Path)
    parser.add_argument('--sdl-source', required=True, type=Path, help='SDL source tree with matching headers and license')
    parser.add_argument('--sdl-library', required=True, type=Path, help='Focal AArch64 SDL3 with KMSDRM and ALSA')
    parser.add_argument('--android-apk', required=True, type=Path, help='Published APK for this version')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', args.version):
        parser.error('expected a release version')
    compiler = Path(build.clang())
    readelf = compiler.with_name('llvm-readelf.exe')
    sysroot = args.sysroot.resolve()
    gcc = sysroot / 'usr/lib/gcc/aarch64-linux-gnu'
    installations = sorted(p for p in gcc.iterdir() if (p / 'crtbegin.o').is_file())
    if not installations:
        parser.error('the sysroot needs AArch64 GCC startup objects')
    STAGE.mkdir(parents=True, exist_ok=True)
    executable = STAGE / 'ss2_linux_arm64'
    subprocess.run([
        str(compiler), '--target=aarch64-linux-gnu', f'--sysroot={sysroot.as_posix()}',
        f'--gcc-install-dir={installations[-1].as_posix()}', '-fuse-ld=lld', '-mcpu=cortex-a35',
        '-O2', '-DSS_REBASED_HOST=1', f'-DSS_HOST_VERSION="{args.version}"',
        '-I', str(ROOT / 'runtime/include'), '-I', str(args.sdl_source / 'include'),
        *[str(ROOT / source) for source in ('runtime/host64/host_linux.c', 'runtime/win32/plat_sdl.c',
                                            'runtime/plat/net_udp.c')],
        str(args.sdl_library), '-lm', '-Wl,-rpath,$ORIGIN/lib', '-o', str(executable),
    ], check=True)
    (STAGE / 'elf-audit.txt').write_text(
        elf_report(readelf, executable, True) + '\n' + elf_report(readelf, args.sdl_library, False),
        encoding='utf-8',
    )
    with zipfile.ZipFile(args.android_apk) as apk:
        image = apk.read('assets/ss2_game.macho')
    if image[:4] != b'\xce\xfa\xed\xfe':
        raise ValueError('Expected the released arm64_32 game image')
    files = {
        'ShiningSource.sh': PORT / 'ShiningSource.sh',
        **{f'shiningsource/{name}': PORT / name for name in ('README.md', 'port.json', 'gameinfo.xml')},
        'shiningsource/ss2_linux_arm64': executable,
        'shiningsource/lib/libSDL3.so.0': args.sdl_library,
        'shiningsource/LICENSE.txt': ROOT / 'LICENSE',
        'shiningsource/SDL3-LICENSE.txt': args.sdl_source / 'LICENSE.txt',
        **{f'shiningsource/assets/launcher/{name}': ROOT / 'assets/launcher' / name for name in SIDE_ART},
    }
    payload = {name: path.read_bytes() for name, path in files.items()}
    payload['ShiningSource.sh'] = payload['ShiningSource.sh'].replace(b'\r\n', b'\n')
    payload['shiningsource/ss2_game.macho'] = image
    payload['shiningsource/roms/README.txt'] = b'Place your unmodified European AU2P cartridge dump here as rom.gba.\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'x', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            member = zipfile.ZipInfo(name)
            member.create_system = 3
            executable_bit = name in ('ShiningSource.sh', 'shiningsource/ss2_linux_arm64')
            member.external_attr = (stat.S_IFREG | (0o755 if executable_bit else 0o644)) << 16
            archive.writestr(member, data, compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(args.output) as archive:
        assert archive.testzip() is None
        assert set(archive.namelist()) == set(payload)
        assert all(archive.read(name) == data for name, data in payload.items())
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    args.output.with_name(args.output.name + '.sha256').write_text(
        f'{digest}  {args.output.name}\n', encoding='ascii', newline='\n',
    )
    print(f'{args.output}\nSHA-256 {digest}\nReleased guest SHA-256 {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
