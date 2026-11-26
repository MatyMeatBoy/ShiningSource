#!/usr/bin/env python3
"""Pack the GA36 (Allwinner A33, ARMv7 hard-float) Ports zip around an ss2_ga36 build.

The zip holds `ShiningSource GA36.sh` and `shiningsourcega36/` with the game, its own loader and
glibc, SDL3 and libasound under libarmhf/, the launcher art and a release manifest. Every bundled
ELF is audited for the stock sunxi 3.4.39 kernel before anything is written.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from launcher_art import SIDE_ART
import ra_build

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'platform/ga36'
ARMHF = Path('D:/01Claude/ReKHCoM/linux/armhf')
TRIPLE = 'arm-linux-gnueabihf'
LOADER = 'ld-linux-armhf.so.3'
INTERPRETER = f'libarmhf/{LOADER}'
GAME = 'shiningsourcega36'
LAUNCHER = 'ShiningSource GA36.sh'
VERSION = '0.1.7-ga36'
STOCK_KERNEL = (3, 4, 39)
# Opened at run time rather than listed as NEEDED: SDL3 dlopens ALSA, and the split glibc
# libraries keep firmware copies (built for another glibc) from being mixed in.
ALWAYS = ('libasound.so.2', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1', LOADER,
          # OpenSSL 3 for RetroAchievements over HTTPS (dlopened by launcher/achievements/ra_https_posix.inc)
          'libssl.so.3', 'libcrypto.so.3')
# SDL3 dlopens these by their unversioned names, which runtime packages do not ship.
PLAIN_ALIASES = {'libasound.so.2': 'libasound.so'}
READELF_HINTS = (os.environ.get('SHININGSOURCE_LLVM_MINGW', ''),
                 'D:/01Claude/toolchain/llvm-mingw-20261006-ucrt-x86_64',
                 'D:/01Claude/ReKHCoM/toolchain/llvm-mingw-20261006-ucrt-x86_64')
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


class AuditError(ValueError):
    pass


def find_readelf(explicit=None):
    if explicit:
        return str(explicit)
    for hint in READELF_HINTS:
        exe = Path(hint) / 'bin/llvm-readelf.exe' if hint else None
        if exe and exe.is_file():
            return str(exe)
    found = shutil.which('llvm-readelf') or shutil.which('readelf')
    if not found:
        raise AuditError('llvm-readelf not found: pass --readelf or set SHININGSOURCE_LLVM_MINGW')
    return found


def is_elf(data):
    return data[:4] == b'\x7fELF'


def check_arm_header(path, data, hard_float=True):
    """ELF32 little-endian ARM, and EABI5 hard-float unless `hard_float` is False."""
    if len(data) < 52 or data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 40:
        raise AuditError(f'{path}: expected a little-endian ARM ELF32 image')
    flags = struct.unpack_from('<I', data, 36)[0]
    if hard_float and flags & 0xFF000600 != 0x05000400:
        raise AuditError(f'{path}: expected ARM EABI5 hard-float (e_flags {flags:#010x})')


def readelf_text(readelf, options, path):
    return subprocess.run([readelf, '--wide', *options, str(path)], check=True, capture_output=True,
                          text=True, encoding='utf-8', errors='replace').stdout


def needed(readelf, path):
    return re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', readelf_text(readelf, ['-d'], path))


def interpreter(readelf, path):
    match = re.search(r'\[Requesting program interpreter: ([^\]]+)\]', readelf_text(readelf, ['-l'], path))
    return match[1] if match else None


def kernel_abi(readelf, path):
    match = re.search(r'OS: Linux, ABI: (\d+)\.(\d+)\.(\d+)', readelf_text(readelf, ['-n'], path))
    return tuple(map(int, match.groups())) if match else None


def glibc_versions(readelf, path):
    return {tuple(map(int, v.split('.'))) for v in
            re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)\b', readelf_text(readelf, ['-V'], path))}


def dotted(version):
    return '.'.join(map(str, version))


def collect_libraries(readelf, roots, search):
    """Bundle name -> source path for the NEEDED closure of `roots` plus ALWAYS."""
    found, todo = {}, list(ALWAYS)
    for root in roots:
        todo += needed(readelf, root)
    while todo:
        name = todo.pop()
        if name in found:
            continue
        source = next((folder / name for folder in search if (folder / name).is_file()), None)
        if source is None:
            raise AuditError(f'runtime library {name} is missing from the sysroot and SDL folders')
        found[name] = source
        todo += needed(readelf, source)
    return found


def git_commit():
    try:
        head = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], check=True,
                              capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=no'],
                               check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'
    return head + ('-dirty' if dirty else '')


def lf(data):
    return data.replace(b'\r\n', b'\n')


def build_payload(args, readelf):
    """Zip member name -> bytes, after the audit."""
    sysroot = args.sysroot.resolve()
    sdl = args.sdl_library.resolve()
    binaries = [args.binary.resolve(), *(p.resolve() for p in args.extra_binary)]
    # The main program is always shipped as ss2_ga36, the name ga36-env.sh starts.
    names = ['ss2_ga36', *(p.name for p in binaries[1:])]
    if len(set(names)) != len(names):
        raise AuditError('two executables share a name')
    for path in [*binaries, sdl, args.sdl_license, sysroot]:
        if not path.exists():
            raise AuditError(f'{path} does not exist')

    report = {}
    for exe in binaries:
        check_arm_header(exe, exe.read_bytes())
        if (interp := interpreter(readelf, exe)) != INTERPRETER:
            raise AuditError(f'{exe}: interpreter {interp!r}, expected {INTERPRETER!r}')

    diagnostic_elves = []
    diagnostic = {}
    if args.diagnostic:
        folder = args.diagnostic.resolve()
        if not folder.is_dir():
            raise AuditError(f'{folder} is not a folder')
        for path in sorted(p for p in folder.rglob('*') if p.is_file()):
            relative = path.relative_to(folder).as_posix()
            data = path.read_bytes()
            if path.suffix == '.sh':
                data = lf(data)
            if is_elf(data):
                check_arm_header(path, data, hard_float=False)
                diagnostic_elves.append(path)
            member = relative if '/' not in relative and path.suffix == '.sh' else f'{GAME}/diagnostic/{relative}'
            diagnostic[member] = data

    search = [sysroot / 'lib' / TRIPLE, sysroot / 'usr/lib' / TRIPLE, sdl.parent]
    libraries = collect_libraries(readelf, [*binaries, sdl, *diagnostic_elves], search)
    libraries['libSDL3.so.0'] = sdl
    for name, source in libraries.items():
        check_arm_header(source, source.read_bytes())
    libc = libraries.get('libc.so.6')
    if libc is None:
        raise AuditError('libc.so.6 is not part of the bundle')
    abi = kernel_abi(readelf, libc)
    if abi is None or abi > STOCK_KERNEL:
        raise AuditError(f'{libc}: kernel ABI {abi and dotted(abi)} needs a newer kernel than '
                         f'{dotted(STOCK_KERNEL)}')
    for path in [*binaries, *libraries.values()]:
        other = kernel_abi(readelf, path)
        if other and other > STOCK_KERNEL:
            raise AuditError(f'{path}: kernel ABI {dotted(other)} is above {dotted(STOCK_KERNEL)}')
    provided = max(glibc_versions(readelf, libc), default=None)
    if provided is None:
        raise AuditError(f'{libc}: no GLIBC version definitions')
    required = {}
    for name, path in [*zip(names, binaries), ('libSDL3.so.0', sdl), ('libasound.so.2', libraries['libasound.so.2'])]:
        required[name] = max(glibc_versions(readelf, path), default=(0,))
        if required[name] > provided:
            raise AuditError(f'{path} needs GLIBC_{dotted(required[name])}, the bundle has '
                             f'GLIBC_{dotted(provided)}')
    report.update(kernel_abi=dotted(abi), glibc=dotted(provided),
                  glibc_required={k: dotted(v) for k, v in sorted(required.items())})

    payload = {
        LAUNCHER: lf((PORT / LAUNCHER).read_bytes()),
        f'{GAME}/ga36-env.sh': lf((PORT / 'ga36-env.sh').read_bytes()),
        f'{GAME}/README.md': lf((PORT / 'README.md').read_bytes()),
        f'{GAME}/gameinfo.xml': lf((PORT / 'gameinfo.xml').read_bytes()),
        f'{GAME}/LICENSE.txt': (ROOT / 'LICENSE').read_bytes(),
        f'{GAME}/SDL3-LICENSE.txt': args.sdl_license.read_bytes(),
        f'{GAME}/roms/README.txt': (
            b'Copy your own unmodified European Shining Soul II cartridge dump (AU2P) here as rom.gba.\n'
            b'Copia aqui tu propio volcado sin modificar del cartucho europeo (AU2P) como rom.gba.\n'),
    }
    for name, exe in zip(names, binaries):
        payload[f'{GAME}/{name}'] = exe.read_bytes()
    for name, source in libraries.items():
        payload[f'{GAME}/libarmhf/{name}'] = source.read_bytes()
    for name, plain in PLAIN_ALIASES.items():
        payload[f'{GAME}/libarmhf/{plain}'] = payload[f'{GAME}/libarmhf/{name}']
    for name in SIDE_ART:
        payload[f'{GAME}/assets/launcher/{name}'] = (ROOT / 'assets/launcher' / name).read_bytes()
    for name, source in ra_build.package_files().items():     # the RetroAchievements set and badges, a CA bundle
        payload[f'{GAME}/{name}'] = source.read_bytes()
    # card linking from a PC (ReKHCoM's format: retroachievements.auth + .ini in this folder)
    payload[f'{GAME}/ra_vincular_cuenta.py'] = lf((PORT / 'ra_vincular_cuenta.py').read_bytes())
    payload[f'{GAME}/Vincular-RetroAchievements-ShiningSource.bat'] = lf(
        (PORT / 'Vincular-RetroAchievements-ShiningSource.bat').read_bytes()).replace(b'\n', b'\r\n')
    alsa = sysroot / 'usr/share/alsa'
    if (alsa / 'alsa.conf').is_file():
        for path in sorted(p for p in alsa.rglob('*') if p.is_file()):
            payload[f'{GAME}/alsa/{path.relative_to(alsa).as_posix()}'] = path.read_bytes()
    clash = set(payload) & set(diagnostic)
    if clash:
        raise AuditError(f'diagnostic files replace package files: {sorted(clash)}')
    payload.update(diagnostic)

    manifest = {
        'name': 'ShiningSource GA36',
        'version': args.version,
        'commit': args.commit or git_commit(),
        'diagnostic': bool(args.diagnostic),
        'audit': report,
        'files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())},
    }
    payload[f'{GAME}/GA36-release.json'] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    return payload, manifest


def member_mode(name, data):
    return 0o755 if name.endswith('.sh') or is_elf(data) else 0o644


def write_zip(payload, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + '.part')
    with zipfile.ZipFile(partial, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(payload):
            member = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            member.create_system = 3
            member.external_attr = (stat.S_IFREG | member_mode(name, payload[name])) << 16
            archive.writestr(member, payload[name], compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(partial) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(payload):
            raise AuditError(f'{partial}: verification failed')
    os.replace(partial, output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + '.sha256').write_text(f'{digest}  {output.name}\n', encoding='ascii',
                                                         newline='\n')
    return digest


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/ga36/ss2/ss2_ga36')
    parser.add_argument('--extra-binary', type=Path, action='append', default=[],
                        help='another executable placed beside ss2_ga36, audited the same way')
    parser.add_argument('--sysroot', type=Path, default=ARMHF / 'sysroot')
    parser.add_argument('--sdl-library', type=Path, default=ARMHF / 'sdl/lib/libSDL3.so.0.4.18')
    parser.add_argument('--sdl-license', type=Path, default=ARMHF / 'sdl-src-LICENSE.txt')
    parser.add_argument('--readelf', help='llvm-readelf (default: the llvm-mingw toolchain or PATH)')
    parser.add_argument('--version', default=VERSION)
    parser.add_argument('--commit', help='commit recorded in GA36-release.json (default: git HEAD)')
    parser.add_argument('--diagnostic', type=Path, metavar='DIR',
                        help='add DIR: top-level *.sh beside the launcher, the rest in shiningsourcega36/diagnostic')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--force', action='store_true', help='replace an existing zip')
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[0-9A-Za-z][0-9A-Za-z.+-]*', args.version):
        parser.error('unexpected version string')
    if args.output is None:
        suffix = '-diagnostic' if args.diagnostic else ''
        args.output = ROOT / f'build/ga36/ShiningSource-GA36-{args.version}{suffix}.zip'
    return parser, args


def main(argv=None):
    parser, args = parse_args(argv)
    if args.output.exists() and not args.force:
        parser.exit(1, f'{args.output} exists (use --force)\n')
    try:
        readelf = find_readelf(args.readelf)
        payload, manifest = build_payload(args, readelf)
        digest = write_zip(payload, args.output)
    except (AuditError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'package_ga36: {error}\n')
    audit = manifest['audit']
    print(f'{args.output}\n{len(payload)} files, glibc {audit["glibc"]}, kernel ABI {audit["kernel_abi"]}\n'
          f'SHA-256 {digest}')


if __name__ == '__main__':
    main()
