#!/usr/bin/env python3
"""Build ShiningSource for the GA36-MB V1.2 handheld (Allwinner A33, Cortex-A7, armhf glibc Linux).

  build_ga36.py [--game ss2] [-j N] [--no-bundled-loader] [--no-headless]

The game is the same PIC ARMv7 rebased guest as the Android ARMv7 build
(tools/build_android32.py: game + runtime/core + private libc), compiled for
arm-linux-gnueabihf with hard float, linked with the same host
(runtime/android32/host.c) against a Debian armhf sysroot and SDL3.

  SHININGSOURCE_ANDROID   NDK folder (default: D:/01Claude/ReKHCoM-build/toolchain/android,
                          then D:/01Claude/ReKHCoM/toolchain/android)
  SHININGSOURCE_ARMHF     folder with sysroot/ and sdl/{include,lib} (default D:/01Claude/ReKHCoM/linux/armhf)

Output, under build/ga36/<game>/:
  <game>_ga36             SDL3 host + guest (KMSDRM/fbdev video, ALSA audio)
  <game>_ga36_headless    no SDL, ROM as argv[1] (for emulator tests)
  libarmhf/               with --bundled-loader (default): the sysroot's loader, glibc and the
                          libraries SDL3 needs; the executables use libarmhf/ld-linux-armhf.so.3
                          (relative to the current folder: the launcher changes into the game folder)
  elf-audit.txt           llvm-readelf report of both executables
No ROM contents are included.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# tools/link.py lists symbols with llvm-mingw's llvm-nm; D:/01Claude/toolchain may hold a dangling link to it.
if 'SHININGSOURCE_LLVM_MINGW' not in os.environ:
    for _hint in ('D:/01Claude/toolchain/llvm-mingw-20261006-ucrt-x86_64',
                  'D:/01Claude/ReKHCoM/toolchain/llvm-mingw-20261006-ucrt-x86_64'):
        if (Path(_hint) / 'bin/clang.exe').is_file():
            os.environ['SHININGSOURCE_LLVM_MINGW'] = _hint
            break
import build
import build_android32
import link
import rebased

ROOT = build.ROOT
VERSION = '0.9.5-ga36-rc3'
TRIPLE = 'arm-linux-gnueabihf'
CPU = ['-mcpu=cortex-a7', '-mfpu=neon-vfpv4', '-mfloat-abi=hard', '-mthumb']
# The Android ARMv7 guest flags with the GA36 target: hard float on both sides
# of the bridge, so b_pow/b_sin/b_cos/b_fmod pass doubles in d0/d1.
TARGET = ['--target=' + TRIPLE, *CPU, '-fPIC', '-ffreestanding', '-fno-short-enums', '-fno-stack-protector',
          '-fno-common', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
LOADER = 'ld-linux-armhf.so.3'
LIBDIR = 'libarmhf'
# Opened by SDL3 at run time (dlopen) besides what it links; GPU drivers stay the system's.
DLOPEN = ['libasound.so.2', 'libudev.so.1', 'libdrm.so.2', 'libgbm.so.1']
# EF_ARM_EABI_VER5 | EF_ARM_ABI_FLOAT_HARD
HARD_FLAGS = 0x05000400


def toolchain():
    if 'SHININGSOURCE_ANDROID' not in os.environ:
        for hint in ('D:/01Claude/ReKHCoM-build/toolchain/android', 'D:/01Claude/ReKHCoM/toolchain/android'):
            if any((Path(hint) / 'native').glob('android-ndk-*')):
                os.environ['SHININGSOURCE_ANDROID'] = hint
                break
    return build_android32.toolchain()


def run(command, description):
    return build_android32.run(command, description)


def guest(game, jobs, cc, tools, out):
    """build_android32.guest() with the GA36 target, output folder and generated-source variant."""
    out.mkdir(parents=True, exist_ok=True)
    runtime = build_android32.prepare_runtime_sources(out)
    resource = run([cc, '-print-resource-dir'], 'clang resource directory').strip()
    own = [
        '-nostdinc', '-isystem', resource + '/include',
        '-I', str(ROOT / 'runtime/guest/include'),
        '-I', str(runtime / 'include'),
        '-I', str(runtime / 'core'),
        '-I', str(ROOT / 'runtime/guest'),
        '-I', str(ROOT / 'runtime/console'), '-DSS_REBASED=1',
    ]
    gen = build.prepare(game, prefix=False, variant='ga36/gen')
    build_android32.prepare_generated_sources(gen)
    replaced = build.port_config(game)['runtime']
    game_flags = [
        flag for flag in build.CFLAGS
        if not flag.startswith('--target') and flag not in ('-g', '-mno-ms-bitfields')
    ]
    game_flags += ['-iquote', str(gen / 'include'), '-I', str(gen / 'include'), *own]
    rt_flags = [*build.RUNTIME_DEFINES, '-O2', '-Wall', *own]
    rt_flags += ['-D%s=__wrap_%s' % (name, name) for name in link.WRAPS]
    units = [
        (path, game_flags, gen, 'game')
        for path in sorted((gen / 'src').glob('*.c')) if path.stem not in replaced
    ]
    units += [(path, rt_flags, None, 'core') for path in sorted((runtime / 'core').glob('*.c'))]
    units += [
        (ROOT / path, rt_flags, None, 'plain') for path in (
            'runtime/guest/guest.c', 'runtime/guest/libc.c',
            'runtime/console/rebase.c', 'runtime/console/co_arm.S',
        )
    ]
    objects = rebased.compile_units(
        str(cc), units, out, TARGET, build_android32.STRAY, rebased.resolver(game), jobs, prefix=False,
    )
    stubs = rebased.tables(game, out, objects, prefix=False, wraps_renamed=True)
    for source in ('stubs.c', 'codetable.c'):
        target = out / 'obj' / (source[:-2] + '.o')
        run([cc, *TARGET, '-O1', '-w', *rt_flags, '-c', out / source, '-o', target], source)
        objects.append(target)
    image = build_android32.combine_objects(objects, out, tools, wraps_renamed=True)
    undefined = sorted({
        line.split()[-1]
        for line in run([tools / 'llvm-nm.exe', '--undefined-only', image], 'undefined names').splitlines()
        if line.split()
    })
    (out / 'guest-needs.txt').write_text('\n'.join(undefined) + '\n', encoding='utf-8')
    print('%s: %d stubbed functions; runtime helpers from libgcc/glibc: %s' % (
        image.relative_to(ROOT), len(stubs), ' '.join(undefined) or '(none)'))
    return image


def check_float_abi(readelf, objects):
    """Every guest object must say VFP-register arguments (Tag_ABI_VFP_args); co_arm.S has no float calls."""
    report = run([readelf, '-A', *objects], 'float ABI attributes')
    soft = [m for m in re.findall(r'TagName: ABI_VFP_args\s*\n\s*Description: ([^\n]+)', report)
            if 'AAPCS VFP' not in m]
    if soft:
        sys.exit('guest objects not built for hard float: ' + ', '.join(sorted(set(soft))))


def host(cc, sysroot, sdl, out, name, guest_o, bundled, headless):
    gcc = sorted(p for p in (sysroot / 'usr/lib/gcc' / TRIPLE).iterdir() if (p / 'crtbegin.o').is_file())
    if not gcc:
        sys.exit('the sysroot needs the armhf GCC startup objects (usr/lib/gcc/%s/*/crtbegin.o)' % TRIPLE)
    flags = ['--target=' + TRIPLE, '--sysroot=' + sysroot.as_posix(), '--gcc-install-dir=' + gcc[-1].as_posix(),
             *CPU, '-fuse-ld=lld', '-O2', '-g', '-Wall', '-DSS_REBASED_HOST=1', '-DSS_HOST_VERSION="%s"' % VERSION,
             '-I', str(ROOT / 'runtime/include')]
    plat = sorted((ROOT / 'runtime/plat').glob('*.c'))
    if headless:
        flags += ['-DSS_ANDROID32_HEADLESS=1']
        sources = [ROOT / 'runtime/android32/host.c', ROOT / 'runtime/host64/plat_null.c',
                   *[p for p in plat if not p.stem.startswith('fbdev')]]
        libraries = ['-lm']
    else:
        flags += ['-DSS_FBDEV=1', '-I', str(sdl / 'include')]
        sources = [ROOT / 'runtime/android32/host.c', ROOT / 'runtime/win32/plat_sdl.c', *plat]
        libraries = ['-L', str(sdl / 'lib'), '-lSDL3', '-lm']
    link_flags = ['-Wl,--build-id', '-Wl,-z,noexecstack', '-Wl,--disable-new-dtags',
                  '-Wl,-rpath,$ORIGIN/' + LIBDIR]
    if bundled:
        link_flags.append('-Wl,--dynamic-linker=%s/%s' % (LIBDIR, LOADER))
    target = out / name
    run([cc, *flags, *sources, guest_o, *libraries, *link_flags, '-o', target], 'host link ' + name)
    return target


def elf_audit(readelf, path, bundled):
    text = run([readelf, '-h', '-l', '-d', '-V', '-A', path], 'llvm-readelf')
    facts = {}
    if not re.search(r'Class:\s+ELF32', text) or not re.search(r'Machine:\s+ARM\b', text):
        sys.exit('%s is not an ELF32 ARM executable' % path.name)
    flags = int(re.search(r'Flags:\s+(0x[0-9a-fA-F]+)', text)[1], 16)
    if flags != HARD_FLAGS:
        sys.exit('%s: ELF flags 0x%08x, expected 0x%08x (EABI5, hard float)' % (path.name, flags, HARD_FLAGS))
    interpreter = re.search(r'Requesting program interpreter: ([^\]]+)\]', text)
    expected = '%s/%s' % (LIBDIR, LOADER) if bundled else '/lib/' + LOADER
    if not interpreter or interpreter[1] != expected:
        sys.exit('%s: interpreter %s, expected %s' % (path.name, interpreter and interpreter[1], expected))
    versions = sorted({tuple(map(int, v.split('.'))) for v in re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)', text)})
    if 'GLIBC_PRIVATE' in text:
        sys.exit('%s needs GLIBC_PRIVATE' % path.name)
    facts['flags'] = '0x%08x' % flags
    facts['interpreter'] = interpreter[1]
    facts['needed'] = re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', text)
    facts['rpath'] = re.findall(r'\((?:RPATH|RUNPATH)\)\s+Library (?:rpath|runpath): \[([^\]]+)\]', text)
    facts['glibc'] = '.'.join(map(str, versions[-1])) if versions else 'none'
    return text, facts


def needed(readelf, path):
    out = run([readelf, '-d', path], 'needed libraries')
    return re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', out)


def stage_libraries(readelf, base, out, executables):
    """libarmhf/: the loader, glibc, SDL3 and what they need, from the sysroot (dereferenced copies)."""
    sysroot = base / 'sysroot'
    search = [sysroot / 'lib' / TRIPLE, sysroot / 'usr/lib' / TRIPLE, base / 'sdl/lib', sysroot / 'lib']
    dest = out / LIBDIR
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    todo = [LOADER, *DLOPEN]
    for executable in executables:
        todo += needed(readelf, executable)
    done, missing = set(), []
    while todo:
        name = todo.pop()
        if name in done:
            continue
        done.add(name)
        source = next((d / name for d in search if (d / name).is_file()), None)
        if source is None:
            missing.append(name)
            continue
        shutil.copyfile(source, dest / name)
        if name != LOADER:
            todo += needed(readelf, dest / name)
    # SDL3 dlopens some libraries by their unversioned name; the bundled copy must win over the system's.
    for name in DLOPEN:
        plain = name.split('.so')[0] + '.so'
        if (dest / name).is_file():
            shutil.copyfile(dest / name, dest / plain)
    return sorted(done - set(missing)), missing


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', default='ss2', choices=sorted(build.GAMES))
    parser.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument('--bundled-loader', action=argparse.BooleanOptionalAction, default=True,
                        help='interpreter libarmhf/%s, rpath $ORIGIN/libarmhf, and stage libarmhf/ (default on)' % LOADER)
    parser.add_argument('--headless', action=argparse.BooleanOptionalAction, default=True,
                        help='also build <game>_ga36_headless (default on)')
    args = parser.parse_args()
    if args.j < 1:
        parser.error('-j must be positive')
    base = Path(os.environ.get('SHININGSOURCE_ARMHF', 'D:/01Claude/ReKHCoM/linux/armhf'))
    sysroot, sdl = base / 'sysroot', base / 'sdl'
    if not (sysroot / 'lib' / TRIPLE).is_dir() or not (sdl / 'lib/libSDL3.so').is_file():
        sys.exit('armhf sysroot/SDL3 not found under %s: set SHININGSOURCE_ARMHF' % base)
    cc, tools = toolchain()
    readelf = tools / 'llvm-readelf.exe'
    out = ROOT / 'build/ga36' / args.game
    out.mkdir(parents=True, exist_ok=True)
    guest_o = guest(args.game, args.j, cc, tools, ROOT / 'build/ga36/native32' / args.game)
    check_float_abi(readelf, [guest_o])
    executables = [host(cc, sysroot, sdl, out, '%s_ga36' % args.game, guest_o, args.bundled_loader, False)]
    if args.headless:
        executables.append(host(cc, sysroot, sdl, out, '%s_ga36_headless' % args.game, guest_o,
                                args.bundled_loader, True))
    report = []
    for executable in executables:
        text, facts = elf_audit(readelf, executable, args.bundled_loader)
        report.append(text)
        print('%s: flags %s, interpreter %s, max GLIBC_%s, rpath %s, NEEDED %s' % (
            executable.relative_to(ROOT), facts['flags'], facts['interpreter'], facts['glibc'],
            ' '.join(facts['rpath']) or '-', ' '.join(facts['needed'])))
    (out / 'elf-audit.txt').write_text('\n'.join(report), encoding='utf-8')
    if args.bundled_loader:
        staged, missing = stage_libraries(readelf, base, out, executables)
        print('%s/: %d libraries%s' % ((out / LIBDIR).relative_to(ROOT), len(staged),
                                        '; not in the sysroot: ' + ' '.join(missing) if missing else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
