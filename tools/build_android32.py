#!/usr/bin/env python3
"""Build the PIC ARMv7 Android guest: build/android/native32/<game>/guest.o.

The game, core, and private guest libc use rebased GBA memory. Only
ss_guest_entry and ss_guest_rom are visible to the Android host bridge.
No ROM contents are included. Requires the Android NDK under SHININGSOURCE_ANDROID.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build
import link
import rebase_ir
import rebased
import stack_args

ROOT = build.ROOT
# Android ARM32 uses AAPCS base (softfp) calling conventions, including bridge
# double arguments. VFPv3-D16 is baseline ARMv7 Android; NEON is not required.
TARGET = [
    '--target=armv7a-linux-androideabi21', '-march=armv7-a',
    '-mfloat-abi=softfp', '-mfpu=vfpv3-d16', '-mthumb', '-fPIC',
    '-ffreestanding', '-fno-short-enums', '-fno-stack-protector',
    '-fno-common', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
]
# Test invalid cartridge addresses, before translation. A broad low-address
# guard would discard legitimate dynamic EWRAM/IWRAM/ROM accesses.
STRAY = rebase_ir.STRAY_RANGES([(0, 0x10000), (0x01000000, 0x02000000)])


def toolchain():
    """Use one NDK LLVM version for both IR rewriting and object generation."""
    android = Path(os.environ.get(
        'SHININGSOURCE_ANDROID', 'D:/01Claude/ReKHCoM-build/toolchain/android'
    ))
    ndk = next((android / 'native').glob('android-ndk-*'), None)
    if ndk is None:
        raise SystemExit('Android NDK not found under ' + str(android / 'native'))
    tools = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    cc = tools / 'clang.exe'
    if not cc.is_file():
        raise SystemExit('Android NDK clang not found: ' + str(cc))
    return cc, tools


def run(command, description):
    done = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if done.returncode:
        raise SystemExit('%s failed:\n%s' % (description, (done.stdout + done.stderr)[-5000:]))
    return done.stdout


def route_wrapped_source(text):
    """Separate game implementations from every call before LLVM can inline them.

    Linker --wrap cannot intercept a call already inlined within one source
    file. Definitions alone become __real_; declarations, direct calls and
    explicit assembler aliases reach __wrap_. This also preserves same-file
    allocator calls, unlike renaming every occurrence to __real_.
    """
    for name in link.WRAPS:
        aliases = re.findall(
            r'\b(\w+)\s*\([^;{}]*\)\s*(?:__asm__|__asm|asm)'
            r'\s*\(\s*"' + re.escape(name) + r'"\s*\)', text,
        )
        for definition in (name, *aliases):
            pattern = (
                r'(^[A-Za-z_][^\n;=]*?\b)' + re.escape(definition)
                + r'(\s*\([^;{]*\)\s*\{)'
            )
            text = re.sub(
                pattern, lambda match: match[1] + '__real_' + name + match[2],
                text, flags=re.M,
            )
        text = re.sub(
            r'((?:__asm__|__asm|asm)\s*\(\s*")' + re.escape(name)
            + r'("\s*\))',
            lambda match: match[1] + '__wrap_' + name + match[2], text,
        )
    macros = ['#define %s __wrap_%s' % (name, name) for name in link.WRAPS]
    return '\n'.join(macros) + '\n' + text


def prepare_generated_sources(gen):
    """Apply guest ABI normalization without Mach-O symbol prefix changes."""
    # AAPCS32 also trusts callers to extend small arguments. Decompiled
    # assembler aliases often declare an int where the definition uses u16;
    # normalize both declarations and definitions, narrowing in the callee.
    pending = {
        path: path.read_text(encoding='utf-8') for path in gen.rglob('*')
        if path.suffix in ('.c', '.h', '.inc')
    }
    report = stack_args.apply(pending)
    (gen / 'stack_args.txt').write_text('\n'.join(report) + '\n', encoding='utf-8')
    for path, text in pending.items():
        if path.suffix == '.c':
            text = route_wrapped_source(text)
        path.write_text(text, encoding='utf-8')


def prepare_runtime_sources(out, source_root=None):
    """Keep normalized core sources and headers private to the ARM32 build."""
    source_root = source_root or ROOT / 'runtime'
    destination = out / 'runtime'
    pending = {}
    for sub in ('core', 'include'):
        for source in (source_root / sub).rglob('*'):
            if source.suffix not in ('.c', '.h', '.inc'):
                continue
            target = destination / source.relative_to(source_root)
            pending[target] = source.read_text(encoding='utf-8')
    # Core code uses stdint names rather than the game's u8/u16 typedefs.
    # Extend recognition only for this sequential preparation pass, restoring
    # the shared helper before any compilation or game source preparation.
    original_narrow = stack_args.NARROW
    try:
        stack_args.NARROW = {
            **{kind: ('uint32_t' if wide == 'u32' else 'int32_t')
               for kind, wide in original_narrow.items()},
            'uint8_t': 'uint32_t', 'uint16_t': 'uint32_t',
            'int8_t': 'int32_t', 'int16_t': 'int32_t',
        }
        report = stack_args.apply(pending)
    finally:
        stack_args.NARROW = original_narrow
    for target, text in pending.items():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')
    (destination / 'stack_args.txt').write_text(
        '\n'.join(report) + '\n', encoding='utf-8',
    )
    return destination


def combine_objects(objects, out, tools, wraps_renamed=False):
    """Resolve wrapper references and hide the guest's private C/runtime names."""
    response = out / 'link.rsp'
    response.write_text(
        '\n'.join('"%s"' % str(obj).replace('\\', '/') for obj in objects),
        encoding='utf-8',
    )
    whole = out / 'guest-all.o'
    run([
        tools / 'ld.lld.exe', '-r',
        *(['--wrap=' + name for name in link.WRAPS] if not wraps_renamed else []),
        '-o', whole, '@' + str(response),
    ], 'ARM32 guest link')
    guest = out / 'guest.o'
    run([
        tools / 'llvm-objcopy.exe', '-G', 'ss_guest_entry', '-G', 'ss_guest_rom',
        whole, guest,
    ], 'ARM32 guest symbol localization')
    return guest


def guest(game, jobs):
    cc, tools = toolchain()
    out = ROOT / 'build/android/native32' / game
    out.mkdir(parents=True, exist_ok=True)
    runtime = prepare_runtime_sources(out)
    resource = run([cc, '-print-resource-dir'], 'clang resource directory').strip()
    own = [
        '-nostdinc', '-isystem', resource + '/include',
        '-I', str(ROOT / 'runtime/guest/include'),
        '-I', str(runtime / 'include'),
        '-I', str(runtime / 'core'),
        '-I', str(ROOT / 'runtime/guest'),
        '-I', str(ROOT / 'runtime/console'), '-DSS_REBASED=1',
    ]
    gen = build.prepare(game, prefix=False, variant='gen-android-armeabi-v7a')
    prepare_generated_sources(gen)
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
    units += [
        (path, rt_flags, None, 'core')
        for path in sorted((runtime / 'core').glob('*.c'))
    ]
    units += [
        (ROOT / path, rt_flags, None, 'plain') for path in (
            'runtime/guest/guest.c', 'runtime/guest/libc.c',
            'runtime/console/rebase.c', 'runtime/console/co_arm.S',
        )
    ]
    objects = rebased.compile_units(
        str(cc), units, out, TARGET, STRAY, rebased.resolver(game), jobs, prefix=False,
    )
    stubs = rebased.tables(game, out, objects, prefix=False, wraps_renamed=True)
    for source in ('stubs.c', 'codetable.c'):
        target = out / 'obj' / (source[:-2] + '.o')
        run([
            cc, *TARGET, '-O1', '-w', *rt_flags, '-c', out / source, '-o', target,
        ], source)
        objects.append(target)
    image = combine_objects(objects, out, tools, wraps_renamed=True)
    undefined = sorted({
        line.split()[-1]
        for line in run([tools / 'llvm-nm.exe', '--undefined-only', image], 'undefined names').splitlines()
        if line.split()
    })
    (out / 'guest-needs.txt').write_text('\n'.join(undefined) + '\n', encoding='utf-8')
    print('%s: %d stubbed functions; unresolved runtime helpers: %s' % (
        image.relative_to(ROOT), len(stubs), ' '.join(undefined) or '(none)',
    ))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', default='ss2', choices=sorted(build.GAMES))
    parser.add_argument('-j', type=int, default=2)
    args = parser.parse_args()
    if args.j < 1:
        parser.error('-j must be positive')
    guest(args.game, args.j)
    return 0


if __name__ == '__main__':
    sys.exit(main())
