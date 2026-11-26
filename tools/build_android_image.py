#!/usr/bin/env python3
"""Build an Android guest whose GBA memory lives inside the image.

Uses the same IR transformations as the console builds. Neither guest needs
the GBA hardware addresses to be available in the Android/ART process.
"""
import argparse
import os
import subprocess
from pathlib import Path

import build
import build_arm64
import build_win64
import link
import rebase_ir
import rebased

ROOT = build.ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', default='ss2', choices=sorted(build.GAMES))
    parser.add_argument('--abi', required=True, choices=('arm64-v8a', 'x86_64'))
    parser.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 4) // 2))
    # A handheld's own image (tools/build_r36s.py): the same game and the same pictures, the runtime tuned for its
    # processor. The cartridge's code keeps -O1 and its flags on every target.
    parser.add_argument('--cpu', help='-mcpu for this image instead of the Android one (e.g. cortex-a35)')
    parser.add_argument('--core-opt', default='2', choices=('2', '3'), help="the runtime's -O level")
    parser.add_argument('--out', type=Path, help='build folder instead of build/android/images/ABI/GAME')
    args = parser.parse_args()
    arm = args.abi == 'arm64-v8a'
    cc = build.clang()
    bindir = Path(cc).parent
    out = args.out.resolve() if args.out else ROOT / 'build/android/images' / args.abi / args.game
    target = [f for f in (build_arm64.GUEST_TARGET if arm else build_win64.GUEST_TARGET) if f != '-fno-pic'] + ['-fPIC']
    if args.cpu:
        target = [f for f in target if not f.startswith('-mcpu=')] + ['-mcpu=' + args.cpu]
    game_flags = [f for f in build.CFLAGS if not f.startswith('--target') and f not in ('-g', '-mno-ms-bitfields')]
    gen = build.prepare(args.game, prefix=False, abi_flags=game_flags + target,
                        variant='gen-android-' + args.abi, prefix_after=arm,
                        wraps=link.WRAPS if arm else ())
    own = ['-I', str(ROOT / 'runtime/guest/include'), '-I', str(ROOT / 'runtime/include'),
           '-I', str(ROOT / 'runtime/guest'), '-I', str(ROOT / 'runtime/console'),
           '-DSS_REBASED=1', '-DSS_REBASED_HOST=1']
    game_flags += ['-iquote', str(gen / 'include'), '-I', str(gen / 'include'), *own]
    rt_flags = [*build.RUNTIME_DEFINES, '-O2', '-Wall', *own]
    if arm:
        rt_flags += ['-D%s=__wrap_%s' % (n, n) for n in link.WRAPS]
    replaced = build.port_config(args.game)['runtime']
    units = [(p, game_flags, gen, 'game') for p in sorted((gen / 'src').glob('*.c')) if p.stem not in replaced]
    units += [(p, rt_flags, None, 'core') for p in sorted((ROOT / 'runtime/core').glob('*.c'))]
    units += [(p, rt_flags, None, 'plain') for p in sorted((ROOT / 'runtime/guest').glob('*.c'))]
    units += [(p, rt_flags, None, 'plain') for p in
              (ROOT / ('runtime/guest/co_a64.S' if arm else 'runtime/guest/co_x64.S'), ROOT / 'runtime/console/rebase.c')]
    # Invalid cartridge addresses only: valid GBA regions must reach ss_arena.
    stray = rebase_ir.STRAY_RANGES([(0, 0x10000), (0x01000000, 0x02000000)])
    objects = rebased.compile_units(cc, units, out, target, stray, rebased.resolver(args.game), args.j, prefix=arm,
                                    core_level='-O' + args.core_opt)
    stubs = rebased.tables(args.game, out, objects, prefix=arm, wraps_renamed=arm)
    for name in ('stubs.c', 'codetable.c'):
        obj = out / 'obj' / (name[:-2] + '.o')
        subprocess.run([cc, *target, *rt_flags, '-w', '-c', str(out / name), '-o', str(obj)], check=True)
        objects.append(obj)
    response = out / 'link.rsp'
    response.write_text('\n'.join('"%s"' % p.as_posix() for p in objects), encoding='utf-8')
    image = out / (args.game + ('_game.macho' if arm else '_game.elf'))
    if arm:
        command = [bindir / 'ld.lld.exe', '-flavor', 'darwin', '-arch', 'arm64_32', '-platform_version', 'watchos', '9.0', '9.0',
                   '-e', '_ss_guest_entry', '-pagezero_size', '0x0A000000', '-pie', '-no_fixup_chains', '-map', out / 'image.map']
    else:
        command = [bindir / 'ld.lld.exe', '-m', 'elf32_x86_64', '-pie', '--no-dynamic-linker', '-T', ROOT / 'runtime/guest/android.ld',
                   *['--wrap=' + n for n in link.WRAPS], '-z', 'noexecstack', '-z', 'max-page-size=0x4000']
    subprocess.run([str(c) for c in [*command, '-o', image, '@' + str(response)]], check=True)
    print('%s: rebased GBA memory; %d functions still stubbed' % (image.relative_to(ROOT), len(stubs)))


if __name__ == '__main__':
    main()
