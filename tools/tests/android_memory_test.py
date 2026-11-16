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
    p.add_argument('--rom', type=Path, required=True)
    args = p.parse_args()
    out = ROOT / 'build/android' / ('blocked-' + args.serial)
    out.mkdir(parents=True, exist_ok=True)
    assert hashlib.sha1(args.rom.read_bytes()).hexdigest() == 'aa1288be9257337abdfe3034898cf6c7aa778fbc'
    cc = args.ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    common = [str(cc), '--target=x86_64-linux-android21', '-U__ANDROID__', '-DSS_REBASED_HOST=1', '-O2', '-Wall',
              '-I' + str(ROOT / 'runtime/include')]
    obj, host = out / 'host.o', out / 'host'
    subprocess.run([*common, '-include', str(ROOT / 'tools/tests/android_block_gba.h'), '-c',
                    str(ROOT / 'runtime/host64/host_linux.c'), '-o', str(obj)], check=True)
    subprocess.run([*common, str(obj), str(ROOT / 'runtime/host64/plat_null.c'),
                    str(ROOT / 'runtime/plat/net_udp.c'), '-o', str(host), '-lm'], check=True)
    with zipfile.ZipFile(args.apk) as z:
        (out / 'ss2_game.elf').write_bytes(z.read('assets/ss2_game.elf'))
    base = [args.adb, '-s', args.serial]
    remote = '/data/local/tmp/ss-memory-regression'
    subprocess.run([*base, 'shell', 'mkdir -p ' + remote], check=True)
    for source, name in ((host, 'host'), (out / 'ss2_game.elf', 'ss2_game.elf'), (args.rom, 'rom.gba'),
                         (ROOT / 'tools/tests/play_input.txt', 'input.txt')):
        subprocess.run([*base, 'push', str(source), remote + '/' + name], check=True)
    result = subprocess.run([*base, 'shell', 'cd ' + remote + ' && chmod 755 host && '
                             'SS_SKIP=0 SS_FRAMES=32000 SS_INPUT_FILE=input.txt ./host rom.gba'],
                            capture_output=True, text=True, timeout=180)
    log = result.stdout + result.stderr
    (out / 'run.log').write_text(log, encoding='utf-8')
    assert 'TEST: original GBA addresses 02000000-0A000000 occupied' in log
    assert 'hash ' + EXPECTED_HASH in log and EXPECTED_AUDIO in log, log[-3000:]
    assert 'heap: allocate' not in log and 'FATAL' not in log, log[-3000:]
    print('Blocked-address regression passed:', args.serial, EXPECTED_HASH)


if __name__ == '__main__':
    main()
