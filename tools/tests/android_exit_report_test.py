"""Real process death -> OS exit record -> durable outbox -> HTTPS ACK.

Only a dedicated disposable emulator: supplies own ROM, kills its own app's
game process, and replaces app test settings. No player report is uploaded.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET

PACKAGE = 'com.shiningsource.android'
FOLDER = '/sdcard/Android/data/' + PACKAGE + '/files'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('adb', 'serial', 'apk', 'rom', 'out'):
        p.add_argument('--' + name, required=True)
    a = p.parse_args()
    assert a.serial.startswith('emulator-')
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)

    def adb(*args, check=True):
        return subprocess.run([a.adb, '-s', a.serial, *args], capture_output=True,
                              text=True, timeout=60, check=check).stdout

    def shell(command, **kwargs):
        return adb('shell', command, **kwargs)

    def play():
        shell('am start -W -n ' + PACKAGE + '/.MainActivity')
        shell('uiautomator dump /sdcard/exit-test-ui.xml')
        ui = ET.fromstring(shell('cat /sdcard/exit-test-ui.xml'))
        node = next(n for n in ui.iter('node') if n.get('text') in ('PLAY', 'JUGAR') and n.get('enabled') == 'true')
        l, t, r, b = map(int, re.findall(r'\d+', node.get('bounds')))
        shell('input tap %d %d' % ((l+r)//2, (t+b)//2))

    def wait_for(predicate, timeout=30):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            value = predicate()
            if value: return value
            time.sleep(.5)
        raise AssertionError('condition not reached')

    assert int(shell('getprop ro.build.version.sdk')) >= 30
    adb('install', '-r', '--abi', 'arm64-v8a', str(Path(a.apk).resolve()))
    shell('am force-stop ' + PACKAGE)
    shell('am start -W -n ' + PACKAGE + '/.MainActivity')
    adb('push', str(Path(a.rom).resolve()), FOLDER + '/rom.gba')
    env = out / 'shiningsource.test.env'
    env.write_text('SS_SKIP=0\nSS_NET=\nSS_IMAGE_MOVE=1\nSS_EXEC_MEMFD=1\nSS_QUIT=6000\n')
    adb('push', str(env), FOLDER + '/shiningsource.test.env')
    # Existing test outbox entries must not be mistaken for this run's receipt.
    shell('run-as ' + PACKAGE + ' rm -rf files/crash-outbox')
    shell('rm -f ' + FOLDER + '/shiningsource.log')
    try:
        play()
        pid = wait_for(lambda: shell('pidof ' + PACKAGE + ':game', check=False).strip())
        wait_for(lambda: 'image loaded' in shell('cat ' + FOLDER + '/shiningsource.log', check=False))
        shell('input keyevent 3')
        shell('run-as ' + PACKAGE + ' kill -9 ' + pid)
        wait_for(lambda: not shell('pidof ' + PACKAGE + ':game', check=False).strip())
        shell('am start -W -n ' + PACKAGE + '/.MainActivity')
        log = wait_for(lambda: (lambda value: value if 'exit-info-id=' in value else '')(
            shell('cat ' + FOLDER + '/shiningsource.log', check=False)))
        (out / 'killed.log').write_text(log)
        assert 'os-exit: Android process exit reason=2 (signaled) status=9' in log, log[-2000:]
        assert 'crash: Android process exit' not in log
        receipts = wait_for(lambda: shell('run-as ' + PACKAGE + ' ls files/crash-outbox', check=False)
                            if '.accepted' in shell('run-as ' + PACKAGE + ' ls files/crash-outbox', check=False) else '', 30)
        assert '.pending' not in receipts
        (out / 'upload-receipts.txt').write_text(receipts)
        # The next game may overwrite current.log only after archiving the old one.
        env.write_text('SS_SKIP=0\nSS_NET=\nSS_FRAMES=600\n')
        adb('push', str(env), FOLDER + '/shiningsource.test.env')
        play()
        wait_for(lambda: 'host64: exit code 0' in shell('cat ' + FOLDER + '/shiningsource.log', check=False))
        previous = shell('cat ' + FOLDER + '/shiningsource.previous.log')
        assert 'os-exit: Android process exit reason=2 (signaled) status=9' in previous
        (out / 'previous.log').write_text(previous)
        current = shell('cat ' + FOLDER + '/shiningsource.log')
        assert 'crash:' not in current and 'process error:' not in current
        result = dict(android_api=int(shell('getprop ro.build.version.sdk')), killed_pid=int(pid),
                      os_signal=9, upload_accepted=True, previous_preserved=True, normal_exit=True)
        (out / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result))
    finally:
        shell('am force-stop ' + PACKAGE, check=False)
        shell('rm -f ' + FOLDER + '/shiningsource.test.env', check=False)


if __name__ == '__main__': main()
