#!/usr/bin/env python3
"""Save/restart regression on a dedicated emulator prepared by android_startup_test.

Removes that test app's existing save. Never use on a player's device.
The game itself writes and verifies the character; then a fresh process reads
the same save. This covers game -> indirect flash callback -> guest file I/O.
"""
import argparse
import hashlib
import re
import subprocess
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from android_startup_test import PACKAGE, FOLDER


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb', required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    def adb(*command, check=True):
        r = subprocess.run([args.adb, '-s', args.serial, *command], capture_output=True, timeout=60)
        if check and r.returncode:
            raise RuntimeError((r.stdout + r.stderr).decode(errors='replace'))
        return r.stdout

    def shell(command, check=True):
        return adb('shell', command, check=check).decode(errors='replace')

    def run(name, frames, save_at=0):
        env = args.out / 'shiningsource.test.env'
        env.write_text('SS_SKIP=0\nSS_INPUT_FILE=play_input.txt\nSS_FRAMES=%d\nSS_TEST_SAVE=%d\n' % (frames, save_at))
        adb('push', str(env), FOLDER + '/shiningsource.test.env')
        shell('am force-stop ' + PACKAGE)
        shell('rm -f ' + FOLDER + '/shiningsource.log')
        shell('am start -W -n ' + PACKAGE + '/.MainActivity')
        shell('uiautomator dump /sdcard/ss-save-ui.xml')
        tree = ET.fromstring(shell('cat /sdcard/ss-save-ui.xml'))
        button = next(n for n in tree.iter('node') if n.get('text') in ('PLAY', 'JUGAR') and n.get('enabled') == 'true')
        left, top, right, bottom = map(int, re.findall(r'\d+', button.get('bounds')))
        shell('input tap %d %d' % ((left + right) // 2, (top + bottom) // 2))
        start = time.monotonic()
        log = ''
        while time.monotonic() - start < 180:
            log = shell('cat ' + FOLDER + '/shiningsource.log', check=False)
            if 'audio:' in log or 'crash:' in log:
                break
            time.sleep(1)
        (args.out / (name + '.log')).write_text(log, encoding='utf-8')
        assert re.search(r'frame\s+%d\s+hash' % frames, log) and 'audio:' in log, log[-3000:]
        assert 'crash:' not in log and 'heap: allocate' not in log, log[-3000:]
        return log

    try:
        shell('am force-stop ' + PACKAGE)
        shell('rm -f ' + FOLDER + '/shiningsource.sav ' + FOLDER + '/shiningsource.sav.bak')
        log = run('save', 15000, 14000)
        assert "save test: the game's own save routine returned 0" in log, log[-3000:]
        adb('pull', FOLDER + '/shiningsource.sav', str(args.out / 'saved.sav'))
        data = (args.out / 'saved.sav').read_bytes()
        assert len(data) == 65536
        run('restart', 120)
        adb('pull', FOLDER + '/shiningsource.sav', str(args.out / 'restarted.sav'))
        assert data == (args.out / 'restarted.sav').read_bytes()
        print('PASS: character saved/verified by game; restart preserves 64 KiB save; SHA256', hashlib.sha256(data).hexdigest())
    finally:
        shell('am force-stop ' + PACKAGE)
        shell('rm -f ' + FOLDER + '/shiningsource.test.env')


if __name__ == '__main__':
    main()
