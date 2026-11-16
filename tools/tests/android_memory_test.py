#!/usr/bin/env python3
"""Run the shipped x86_64 image with every original GBA address inaccessible.

Requires a dedicated x86_64 Android emulator, an NDK and the final APK. No ROM
is included: pass your own verified Europe dump. Outputs go under build/android.
"""
import argparse
import hashlib
import subprocess
import zipfile
from pathlib import Path

from android_startup_test import EXPECTED_HASH, EXPECTED_AUDIO

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb', required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--ndk', type=Path, required=True)
    p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--image', type=Path, help='development override: test a newly linked x32 image before packaging')
    p.add_argument('--rom', type=Path, required=True)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--block-stacks', action='store_true', help='occupy former stacks/bridge and the first allocation hint')
    mode.add_argument('--exhaust-stacks', action='store_true', help='occupy all 32-bit stack space; require a clean startup failure')
    mode.add_argument('--stack-hole', action='store_true', help='leave only exact-size stack/bridge holes below the first hint')
    mode.add_argument('--block-image', action='store_true', help='occupy the entire original GBA/image/stack/bridge window')
    args = p.parse_args()
    prefix = ('blocked-image-' if args.block_image else 'stack-hole-' if args.stack_hole else 'exhausted-stacks-' if args.exhaust_stacks
              else 'blocked-stacks-' if args.block_stacks else 'blocked-')
    out = ROOT / 'build/android' / (prefix + args.serial)
    out.mkdir(parents=True, exist_ok=True)
    assert hashlib.sha1(args.rom.read_bytes()).hexdigest() == 'aa1288be9257337abdfe3034898cf6c7aa778fbc'
    cc = args.ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    common = [str(cc), '--target=x86_64-linux-android21', '-U__ANDROID__', '-DSS_REBASED_HOST=1', '-O2', '-Wall',
              '-I' + str(ROOT / 'runtime/include')]
    obj, host = out / 'host.o', out / 'host'
    fixture = 'android_block_stacks.h' if args.block_stacks or args.exhaust_stacks or args.stack_hole else 'android_block_gba.h'
    if args.block_image:
        fixture = 'android_block_image.h'
    subprocess.run([*common, '-include', str(ROOT / 'tools/tests' / fixture), '-c',
                    str(ROOT / 'runtime/host64/host_linux.c'), '-o', str(obj)], check=True)
    subprocess.run([*common, str(obj), str(ROOT / 'runtime/host64/plat_null.c'),
                    str(ROOT / 'runtime/plat/net_udp.c'), '-o', str(host), '-lm'], check=True)
    if args.image:
        (out / 'ss2_game.elf').write_bytes(args.image.read_bytes())
    else:
        with zipfile.ZipFile(args.apk) as z:
            (out / 'ss2_game.elf').write_bytes(z.read('assets/ss2_game.elf'))
    base = [args.adb, '-s', args.serial]
    remote = '/data/local/tmp/ss-memory-regression'
    subprocess.run([*base, 'shell', 'mkdir -p ' + remote], check=True)
    for source, name in ((host, 'host'), (out / 'ss2_game.elf', 'ss2_game.elf'), (args.rom, 'rom.gba'),
                         (ROOT / 'tools/tests/play_input.txt', 'input.txt')):
        subprocess.run([*base, 'push', str(source), remote + '/' + name], check=True)
    extra_env = 'SS_TEST_STACK_HOLE=1 ' if args.stack_hole else 'SS_TEST_EXHAUST_STACKS=1 ' if args.exhaust_stacks else ''
    command = ('cd ' + remote + ' && chmod 755 host && ' + extra_env +
               'SS_SKIP=0 SS_FRAMES=32000 SS_INPUT_FILE=input.txt ./host rom.gba')
    # Android 5's shell transport does not propagate the remote exit code.
    command += '; status=$?; echo SS_TEST_EXIT:$status; exit $status'
    result = subprocess.run([*base, 'shell', command],
                            capture_output=True, text=True, timeout=180)
    log = result.stdout + result.stderr
    (out / 'run.log').write_text(log, encoding='utf-8')
    if args.exhaust_stacks:
        assert 'SS_TEST_EXIT:3' in log and result.returncode in (0, 3), log[-3000:]
        assert 'TEST: all 32-bit stack space occupied' in log
        assert 'no free 32-bit guest range' in log
        assert 'image loaded' not in log and 'crash:' not in log
        print('Exhausted 32-bit stack space: clean failure without pointer truncation:', args.serial)
        return
    assert result.returncode == 0 and 'SS_TEST_EXIT:0' in log, log[-3000:]
    if args.block_image:
        assert 'TEST: original GBA/image/stack/bridge window occupied' in log
        assert 'TEST: occupied image mapping preserved' in log
        assert 'image moved' in log
    elif args.stack_hole:
        assert 'TEST: only stack/bridge holes below first hint available' in log
        assert 'guest map 15000000-19400000' in log
        assert 'guest map 1A000000-1A010000' in log
    elif args.block_stacks:
        assert 'TEST: old stacks/bridge and first allocation hint occupied' in log
        assert 'TEST: occupied stack mappings preserved' in log
    else:
        assert 'TEST: original GBA addresses 02000000-0A000000 occupied' in log
    assert 'hash ' + EXPECTED_HASH in log and EXPECTED_AUDIO in log, log[-3000:]
    assert 'heap: allocate' not in log and 'FATAL' not in log, log[-3000:]
    print('Blocked-address regression passed:', args.serial, EXPECTED_HASH)


if __name__ == '__main__':
    main()
