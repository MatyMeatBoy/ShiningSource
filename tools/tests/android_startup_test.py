#!/usr/bin/env python3
"""Exercise a --test APK on a dedicated Android test device, including cold starts.

Overwrites the test app's ROM/settings and removes its saves. Do not use on a
player's device. Supply your own ROM and the canonical 32,000-frame input file.
Example: --serial emulator-5554 --abi arm64-v8a --apk build/android/ShiningSource-debug.apk
         --rom PATH --input tools/tests/play_input.txt --out build/android/qa-arm64
"""
import argparse
import hashlib
import json
import re
import subprocess
import time
import xml.etree.ElementTree as ET
from pathlib import Path

PACKAGE = 'com.shiningsource.android'
FOLDER = '/sdcard/Android/data/' + PACKAGE + '/files'
EXPECTED_HASH = '5F1DB1A8'
EXPECTED_AUDIO = 'audio: 25716797 sample frames, peak 10279, 35889340 samples above the noise floor'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--adb', required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--abi', choices=('x86_64', 'arm64-v8a'), required=True)
    p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--launcher', action='store_true', help='press PLAY on MainActivity; works with the final APK')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    def adb(*command, check=True):
        result = subprocess.run([args.adb, '-s', args.serial, *command], capture_output=True, text=True, timeout=60)
        if check and result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def shell(command, check=True):
        return adb('shell', command, check=check)

    assert hashlib.sha1(args.rom.read_bytes()).hexdigest() == 'aa1288be9257337abdfe3034898cf6c7aa778fbc', 'unsupported test ROM'
    adb('install', '-r', '--abi', args.abi, str(args.apk))
    shell('mkdir -p ' + FOLDER)
    adb('push', str(args.rom), FOLDER + '/rom.gba')
    adb('push', str(args.input), FOLDER + '/play_input.txt')
    env = args.out / 'shiningsource.test.env'
    env.write_text('SS_SKIP=0\nSS_FRAMES=32000\nSS_INPUT_FILE=play_input.txt\nSS_HASH_ALL=1\n', encoding='utf-8')
    adb('push', str(env), FOLDER + '/shiningsource.test.env')
    info = {key: shell('getprop ' + key).strip() for key in
            ('ro.build.version.release', 'ro.build.version.sdk', 'ro.product.model', 'ro.product.cpu.abilist')}
    page_size = shell('getconf PAGE_SIZE', check=False).strip()
    info.update(abi=args.abi, page_size=page_size if page_size.isdigit() else 'unknown',
                apk_sha256=hashlib.sha256(args.apk.read_bytes()).hexdigest(), runs=[])
    package = shell('dumpsys package ' + PACKAGE)
    assert 'primaryCpuAbi=' + args.abi in package, 'wrong native ABI installed'
    (args.out / 'package.txt').write_text(package, encoding='utf-8')
    shell('input keyevent 224')
    shell('wm dismiss-keyguard', check=False)

    def launcher_ui():
        for attempt in range(4):
            try:
                shell('uiautomator dump /sdcard/ss-qa-ui.xml')
                ui = shell('cat /sdcard/ss-qa-ui.xml')
                ET.fromstring(ui)
                return ui
            except (RuntimeError, ET.ParseError):
                if attempt == 3:
                    raise
                time.sleep(1)
    try:
        for run in range(args.runs):
            shell('am force-stop ' + PACKAGE)
            shell('rm -f ' + FOLDER + '/shiningsource.log ' + FOLDER + '/shiningsource.sav ' + FOLDER + '/shiningsource.sav.bak')
            start = time.monotonic()
            if args.launcher:
                shell('am start -W -n ' + PACKAGE + '/.MainActivity')
                ui = launcher_ui()
                # First launch asks the player to choose a screen mode before
                # enabling PLAY. Exercise that choice as part of startup.
                modes = [node for node in ET.fromstring(ui).iter('node')
                         if node.get('class') == 'android.widget.RadioButton'
                         and node.get('text', '').startswith(('Classic:', 'Clásico:'))]
                if modes and modes[0].get('checked') != 'true':
                    left, top, right, bottom = map(int, re.findall(r'\d+', modes[0].get('bounds')))
                    shell('input tap %d %d' % ((left + right) // 2, (top + bottom) // 2))
                    ui = launcher_ui()
                online = [node for node in ET.fromstring(ui).iter('node')
                          if node.get('class') == 'android.widget.CheckBox'
                          and node.get('checked') == 'true']
                if online:
                    left, top, right, bottom = map(int, re.findall(r'\d+', online[0].get('bounds')))
                    shell('input tap %d %d' % ((left + right) // 2, (top + bottom) // 2))
                    ui = launcher_ui()
                (args.out / ('launcher-%d.xml' % (run + 1))).write_text(ui, encoding='utf-8')
                buttons = [node for node in ET.fromstring(ui).iter('node')
                           if node.get('text') in ('PLAY', 'JUGAR') and node.get('enabled') == 'true']
                assert len(buttons) == 1, 'launcher PLAY not available; inspect saved UI dump'
                left, top, right, bottom = map(int, re.findall(r'\d+', buttons[0].get('bounds')))
                shell('input tap %d %d' % ((left + right) // 2, (top + bottom) // 2))
            else:
                shell('am start -n ' + PACKAGE + '/.GameActivity')
            log = ''
            while time.monotonic() - start < 180:
                log = shell('cat ' + FOLDER + '/shiningsource.log', check=False)
                if EXPECTED_AUDIO in log or 'FATAL' in log or 'memory map ' in log:
                    break
                time.sleep(1)
            (args.out / ('run-%d.log' % (run + 1))).write_text(log, encoding='utf-8')
            pages = re.search(r'rebased GBA memory; page size (\d+)', log)
            assert pages, 'missing native page-size diagnostic'
            info['page_size'] = pages[1]  # sysconf also works on Android 5, which has no getconf utility
            assert re.search(r'frame\s+32000\s+hash ' + EXPECTED_HASH, log), 'gameplay hash failed; inspect saved log'
            assert EXPECTED_AUDIO in log, 'audio regression; inspect saved log'
            assert 'heap: allocate' not in log and 'FATAL' not in log, 'runtime error; inspect saved log'
            info['runs'].append({'run': run + 1, 'hash': EXPECTED_HASH, 'audio': EXPECTED_AUDIO, 'seconds': round(time.monotonic() - start, 2)})
            print('%s run %d passed' % (args.abi, run + 1), flush=True)
    finally:
        shell('am force-stop ' + PACKAGE)
        shell('rm -f ' + FOLDER + '/shiningsource.test.env')
        (args.out / 'results.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
