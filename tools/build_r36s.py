#!/usr/bin/env python3
"""Build ShiningSource for the R36S (Rockchip RK3326, 4 x Cortex-A35, 64-bit Linux firmware) and pack its Ports zip.

  build_r36s.py [--version 0.9.6] [-j N] [--image FILE] [--no-package] [--output ZIP]

The game is this tree's ARM64 game image (tools/build_android_image.py --abi arm64-v8a: the rebased arm64_32
Mach-O the Android build ships, here with its runtime at -O3 for the Cortex-A35: IMAGE_FLAGS), built again here
every time, never taken from a published APK. The host is
runtime/host64/host_linux.c with the SDL3 platform layer (runtime/win32/plat_sdl.c) and runtime/plat/*.c, compiled
for aarch64-linux-gnu against an Ubuntu 20.04 sysroot (glibc 2.31) and the system's own loader, as the GA36 build
is with SS_FBDEV=1: the picture goes to the panel through DRM/KMS with three buffers (runtime/plat/fbdev_linux.c),
SDL runs offscreen for the controller and sound (ALSA). SDL3's KMSDRM driver stays available (SS_FBDEV empty).

  SHININGSOURCE_R36S_SYSROOT  AArch64 Ubuntu 20.04 sysroot with GCC 10 startup objects
                              (default D:/01Claude/Dynabrothers1+2/local/darkos-vm/focal)
  SHININGSOURCE_R36S_SDL      that sysroot's SDL3 3.4.18 libSDL3.so.0 with KMSDRM, offscreen and ALSA
                              (default D:/01Claude/Dynabrothers1+2/build/r36s/stage-alpha3/lib/libSDL3.so.0)
  SHININGSOURCE_SDL3          the matching SDL3 source tree (headers and licence; default
                              D:/01Claude/ReKHCoM/toolchain/android/SDL3-3.4.18)

Output, under build/r36s/: ss2_r36s (SDL3 host), ss2_r36s_headless (no SDL: ROM as argv[1], for emulator tests),
ss2_game.macho, elf-audit.txt; and the package build/releases/<version>/ShiningSource-<version>-r36s-arm64.zip
(+ .sha256). Every executable is checked to need nothing newer than GLIBC_2.30 (Ubuntu 19.10 based ArkOS; 20.04
and Debian based firmwares are newer). No ROM contents are included.
"""

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
if 'SHININGSOURCE_LLVM_MINGW' not in os.environ:
    for _hint in ('D:/01Claude/toolchain/llvm-mingw-20261006-ucrt-x86_64',
                  'D:/01Claude/ReKHCoM/toolchain/llvm-mingw-20261006-ucrt-x86_64'):
        if (Path(_hint) / 'bin/clang.exe').is_file():
            os.environ['SHININGSOURCE_LLVM_MINGW'] = _hint
            break
import build
import ra_build
from launcher_art import SIDE_ART

ROOT = build.ROOT
PORT = ROOT / 'platform/r36s'
STAGE = ROOT / 'build/r36s'
VERSION = '0.9.6'
GAME = 'shiningsource'
LAUNCHER = 'ShiningSource.sh'
GLIBC_LIMIT = (2, 30)
CPU = ['-mcpu=cortex-a35']
HOST_OPT = ['-O2']
IMAGE_FLAGS = ['--cpu', 'cortex-a35', '--core-opt', '3']
DEFAULTS = {
    'SHININGSOURCE_R36S_SYSROOT': 'D:/01Claude/Dynabrothers1+2/local/darkos-vm/focal',
    'SHININGSOURCE_R36S_SDL': 'D:/01Claude/Dynabrothers1+2/build/r36s/stage-alpha3/lib/libSDL3.so.0',
    'SHININGSOURCE_SDL3': 'D:/01Claude/ReKHCoM/toolchain/android/SDL3-3.4.18',
}
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


def setting(name):
    return Path(os.environ.get(name, DEFAULTS[name]))


def run(command, what):
    done = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if done.returncode:
        print((done.stdout + done.stderr)[-4000:])
        sys.exit('failed: ' + what)
    return done.stdout


def elf_audit(readelf, path, executable):
    text = run([readelf, '--wide', '-h', '-l', '-d', '-V', path], 'llvm-readelf ' + path.name)
    if not re.search(r'Machine:\s+AArch64', text) or not re.search(r'Class:\s+ELF64', text):
        sys.exit(f'{path} is not an AArch64 ELF64 file')
    if executable and 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' not in text:
        sys.exit(f'{path}: unexpected program interpreter')
    versions = [tuple(map(int, v.split('.'))) for v in re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)', text)]
    newest = max(versions, default=(0,))
    if newest > GLIBC_LIMIT or 'GLIBC_PRIVATE' in text:
        sys.exit(f'{path} needs GLIBC_{".".join(map(str, newest))}: above {".".join(map(str, GLIBC_LIMIT))}')
    needed = re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', text)
    return text, newest, needed


def guest(jobs, explicit):
    """The ARM64 game image of this tree (always built again unless --image names one)."""
    if explicit:
        image = explicit.resolve()
    else:
        # its own build of the Android image: the runtime (renderer, picture finishing) at -O3 for the Cortex-A35
        # (about 7 % fewer instructions for the picture); the cartridge's code is compiled as for Android, so the
        # game and its pictures are the same
        done = subprocess.run([sys.executable, str(ROOT / 'tools/build_android_image.py'), '--abi', 'arm64-v8a',
                               '-j', str(jobs), *IMAGE_FLAGS, '--out', str(STAGE / 'image')])
        image = STAGE / 'image/ss2_game.macho'
        if done.returncode or not image.is_file():
            sys.exit('the ARM64 game image could not be built')
    data = image.read_bytes()
    if data[:4] != b'\xce\xfa\xed\xfe':
        sys.exit(f'{image} is not the arm64_32 game image')
    return data


def host(cc, sysroot, sdl_source, sdl_library, name, headless, version):
    gcc = sorted(p for p in (sysroot / 'usr/lib/gcc/aarch64-linux-gnu').iterdir() if (p / 'crtbegin.o').is_file())
    if not gcc:
        sys.exit('the sysroot needs the AArch64 GCC startup objects (usr/lib/gcc/aarch64-linux-gnu/*/crtbegin.o)')
    flags = ['--target=aarch64-linux-gnu', '--sysroot=' + sysroot.as_posix(), '--gcc-install-dir=' + gcc[-1].as_posix(),
             *CPU, '-fuse-ld=lld', *HOST_OPT,'-g', '-Wall', '-DSS_REBASED_HOST=1', '-DSS_HOST_VERSION="%s"' % version,
             '-I', str(ROOT / 'runtime/include'), '-I', str(sdl_source / 'include')]
    if headless:
        sources = ['runtime/host64/host_linux.c', 'runtime/host64/plat_null.c', 'runtime/plat/net_udp.c']
        libraries = [sdl_library, '-lm', '-lpthread']     # (host_linux.c checks the ROM with SDL_crc32)
    else:
        # with the achievements (tools/ra_build.py): HTTPS through the firmware's own OpenSSL (libssl.so.1.1 on
        # Ubuntu 20.04 based ones), opened at run time
        flags += ['-DSS_FBDEV=1', *ra_build.flags(handheld=True)]
        sources = ['runtime/host64/host_linux.c', 'runtime/win32/plat_sdl.c',
                   *sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / 'runtime/plat').glob('*.c')),
                   *(p.relative_to(ROOT).as_posix() for p in ra_build.sources())]
        libraries = [sdl_library, '-lm', '-lpthread', *ra_build.LIBRARIES]
    target = STAGE / name
    run([cc, *flags, *[ROOT / s for s in sources], *libraries, '-Wl,--build-id', '-Wl,-z,noexecstack',
         '-Wl,--disable-new-dtags', '-Wl,-rpath,$ORIGIN/lib', '-o', target], 'host link ' + name)
    return target


def git_commit():
    try:
        head = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], check=True, capture_output=True,
                              text=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=no'],
                               check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'
    return head + ('-dirty' if dirty else '')


def lf(data):
    return data.replace(b'\r\n', b'\n')


def package(version, executables, image, sdl_source, sdl_library, audit, output):
    payload = {
        LAUNCHER: lf((PORT / LAUNCHER).read_bytes()),
        f'{GAME}/r36s-env.sh': lf((PORT / 'r36s-env.sh').read_bytes()),
        f'{GAME}/README.md': lf((PORT / 'README.md').read_bytes()),
        f'{GAME}/port.json': lf((PORT / 'port.json').read_bytes()),
        f'{GAME}/gameinfo.xml': lf((PORT / 'gameinfo.xml').read_bytes()),
        f'{GAME}/LICENSE.txt': (ROOT / 'LICENSE').read_bytes(),
        f'{GAME}/SDL3-LICENSE.txt': (sdl_source / 'LICENSE.txt').read_bytes(),
        f'{GAME}/lib/libSDL3.so.0': sdl_library.read_bytes(),
        f'{GAME}/ss2_game.macho': image,
        f'{GAME}/roms/README.txt': (
            b'Copy your own unmodified European Shining Soul II cartridge dump (AU2P) here as rom.gba.\n'
            b'Copia aqui tu propio volcado sin modificar del cartucho europeo (AU2P) como rom.gba.\n'),
    }
    for path in executables:
        payload[f'{GAME}/{path.name}'] = path.read_bytes()
    for name in SIDE_ART:
        payload[f'{GAME}/assets/launcher/{name}'] = (ROOT / 'assets/launcher' / name).read_bytes()
    for name, source in ra_build.package_files().items():     # the RetroAchievements set and badges, a CA bundle
        payload[f'{GAME}/{name}'] = source.read_bytes()
    assert not any(Path(n).suffix.lower() in ('.gba', '.sav') for n in payload), 'a ROM or a save must never be packaged'
    manifest = {
        'name': 'ShiningSource R36S',
        'version': version,
        'commit': git_commit(),
        'audit': audit,
        'files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())},
    }
    payload[f'{GAME}/R36S-release.json'] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + '.part')
    with zipfile.ZipFile(partial, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(payload):
            member = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            member.create_system = 3
            data = payload[name]
            executable = name.endswith('.sh') or data[:4] == b'\x7fELF' and not name.endswith('.so.0')
            member.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            archive.writestr(member, data, compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(partial) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(payload):
            sys.exit(f'{partial}: verification failed')
        if any(archive.read(name) != data for name, data in payload.items()):
            sys.exit(f'{partial}: contents differ')
    os.replace(partial, output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + '.sha256').write_text(f'{digest}  {output.name}\n', encoding='ascii', newline='\n')
    return digest, len(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', default=VERSION)
    parser.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument('--image', type=Path, help='use this ss2_game.macho instead of building it again')
    parser.add_argument('--no-package', action='store_true', help='only build build/r36s/')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--force', action='store_true', help='replace an existing zip')
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?(?:-[0-9A-Za-z.]+)?', args.version):
        parser.error('expected a release version')
    sysroot, sdl_library, sdl_source = (setting(n).resolve() for n in
                                        ('SHININGSOURCE_R36S_SYSROOT', 'SHININGSOURCE_R36S_SDL', 'SHININGSOURCE_SDL3'))
    if not (sysroot / 'usr/include/stdio.h').is_file():
        sys.exit(f'no AArch64 sysroot at {sysroot}: set SHININGSOURCE_R36S_SYSROOT')
    if not sdl_library.is_file() or not (sdl_source / 'include/SDL3/SDL.h').is_file():
        sys.exit('SDL3 not found: set SHININGSOURCE_R36S_SDL (the library) and SHININGSOURCE_SDL3 (its source tree)')
    output = args.output or ROOT / f'build/releases/{args.version}/ShiningSource-{args.version}-r36s-arm64.zip'
    if not args.no_package and output.exists() and not args.force:
        parser.error(f'{output} exists (use --force)')
    cc = Path(build.clang())
    readelf = cc.with_name('llvm-readelf.exe')
    STAGE.mkdir(parents=True, exist_ok=True)
    image = guest(args.j, args.image)
    (STAGE / 'ss2_game.macho').write_bytes(image)
    executables = [host(cc, sysroot, sdl_source, sdl_library, 'ss2_r36s', False, args.version),
                   host(cc, sysroot, sdl_source, sdl_library, 'ss2_r36s_headless', True, args.version)]
    report, audit = [], {'glibc_required': {}, 'needed': {}}
    for path, executable in [*((p, True) for p in executables), (sdl_library, False)]:
        text, newest, needed = elf_audit(readelf, path, executable)
        name = 'libSDL3.so.0' if path == sdl_library else path.name
        report.append(text)
        audit['glibc_required'][name] = '.'.join(map(str, newest))
        audit['needed'][name] = needed
        print(f'{name}: max GLIBC_{".".join(map(str, newest))}, NEEDED {" ".join(needed)}')
    (STAGE / 'elf-audit.txt').write_text('\n'.join(report), encoding='utf-8')
    print(f'game image: {len(image)} bytes, SHA-256 {hashlib.sha256(image).hexdigest()}')
    if args.no_package:
        return 0
    digest, count = package(args.version, executables, image, sdl_source, sdl_library, audit, output)
    print(f'{output}\n{count} files, SHA-256 {digest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
