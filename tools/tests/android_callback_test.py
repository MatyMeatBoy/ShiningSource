"""Execute portable scene callbacks between Windows and a relocated ARM64 APK.

Dedicated disposable emulator only; overwrites its app test data. Supply your
own EU ROM. Injects A3/A4 callbacks whose observable effect is song0x62, then
continues the game's normal multiplayer scene route. Never use a public relay.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = 'com.shiningsource.android'
FOLDER = '/sdcard/Android/data/' + PACKAGE + '/files'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb', required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--rom', required=True, type=Path)
    p.add_argument('--apk', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--nm', required=True)
    p.add_argument('--map-exits', action='store_true', help='exercise natural map exits instead of injecting callbacks')
    a = p.parse_args()
    assert a.serial.startswith('emulator-')
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    win = out / 'windows'; win.mkdir(exist_ok=True)

    def adb(*args, timeout=180, check=True):
        return subprocess.run([a.adb, '-s', a.serial, *args], capture_output=True,
                              text=True, timeout=timeout, check=check)

    def shell(command, **kwargs):
        return adb('shell', command, **kwargs).stdout

    assert shell('getprop sys.boot_completed').strip() == '1'
    symbols = subprocess.check_output([a.nm, str(ROOT / 'build/win64/ss2/ss2_game.elf')], text=True)
    native = int(re.search(r'^([0-9a-fA-F]+) T sub_08006E58$', symbols, re.M)[1], 16)
    injected = '28000:' + (b'\xA3\x01' + struct.pack('<II', native, 0)).hex()
    injected += ',28100:' + (b'\xA4\x02' + struct.pack('<IIII', native, 0, 30, 0)).hex()
    events = [(int(f), int(b, 16)) for f, b in re.findall(r'(\d+):([0-9A-Fa-f]+)',
              (ROOT / 'tools/tests/net_outage_input.txt').read_text()) if int(f) < 23400]
    for at in (23410, 23440, 23470): events.extend(((at, 0x200), (at + 6, 0)))
    for at in range(23500, 42000, 40): events.extend(((at, 1), (at + 6, 0)))
    script = out / 'input.txt'; script.write_text(','.join('%d:%04X' % e for e in sorted(events)))
    room = 'callbacks-' + str(time.time_ns())
    settings = dict(SS_NET_ROOM=room, SS_NET_DELAY='4', SS_NET_PLAYERS='2', SS_SKIP='0',
                    SS_FRAMES='42000', SS_PACE='1', SS_PACE_FAST='42000:12',
                    SS_INPUT_HUNT='23500', SS_INPUT_GOTO='25200:120:100:41000',
                    SS_NET_MSG_LOG='msg.log', SS_NET_MSG_DUMP='1', SS_M4A_LOG='1')
    if a.map_exits:
        injected = ''
        settings.update(SS_INPUT_GOTO='25200:120:100:31000', SS_INPUT_EXIT='31000',
                        SS_INPUT_PATH='1', SS_STATE_LOG='state.log', SS_SCENE_LOG='scene.log')
    adb('install', '-r', '--abi', 'arm64-v8a', str(a.apk))
    shell('am start -W -n ' + PACKAGE + '/.MainActivity')
    shell('mkdir -p ' + FOLDER)
    adb('push', str(a.rom.resolve()), FOLDER + '/rom.gba')
    adb('push', str(script), FOLDER + '/callback-input.txt')
    env_file = out / 'shiningsource.test.env'
    android = dict(settings, SS_NET='10.0.2.2:47964', SS_INPUT_FILE='callback-input.txt',
                   SS_IMAGE_MOVE='1', SS_EXEC_MEMFD='1')
    env_file.write_text(''.join(k + '=' + v + '\n' for k, v in android.items()))
    adb('push', str(env_file), FOLDER + '/shiningsource.test.env')
    shell('am force-stop ' + PACKAGE)
    shell('rm -f ' + FOLDER + '/shiningsource.sav ' + FOLDER + '/shiningsource.sav.bak ' + FOLDER + '/shiningsource.log ' + FOLDER + '/msg.log')
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    relay = peer = None
    try:
        with (out / 'relay.log').open('w') as relay_log, (win / 'run.log').open('w') as win_log:
            relay = subprocess.Popen([str(ROOT / 'build/relay/ss_relay.exe'), '47964'], stdout=relay_log, stderr=subprocess.STDOUT, creationflags=flags)
            time.sleep(.4)
            env = {k: v for k, v in os.environ.items() if not k.startswith('SS_')}
            env.update(settings, SS_NET='127.0.0.1:47964', SS_INPUT_FILE=str(script), SS_NET_TEST_MSG=injected)
            peer = subprocess.Popen([str(ROOT / 'build/win64/ss2/ss2_win64.exe'), str(a.rom.resolve())], cwd=win, env=env, stdout=win_log, stderr=subprocess.STDOUT, creationflags=flags)
            shell('am start -W -n ' + PACKAGE + '/.MainActivity')
            shell('uiautomator dump /sdcard/callback-ui.xml')
            ui = shell('cat /sdcard/callback-ui.xml'); (out / 'launcher.xml').write_text(ui)
            button = next(n for n in ET.fromstring(ui).iter('node') if n.get('text') in ('PLAY', 'JUGAR') and n.get('enabled') == 'true')
            l, t, r, b = map(int, re.findall(r'\d+', button.get('bounds')))
            shell('input tap %d %d' % ((l + r) // 2, (t + b) // 2))
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                log = shell('cat ' + FOLDER + '/shiningsource.log', check=False)
                if 'audio:' in log or 'crash:' in log or 'stopped safely' in log: break
                time.sleep(1)
            (out / 'android.log').write_text(log)
            assert 'frame 42000' in log and 'crash:' not in log and 'stopped safely' not in log, log[-2000:]
            assert peer.wait(timeout=30) == 0
        adb('pull', FOLDER + '/msg.log', str(out / 'android-msg.log'))
        if a.map_exits:
            for name in ('state.log', 'scene.log'):
                adb('pull', FOLDER + '/' + name, str(out / ('android-' + name)))
        logs = [(out / 'android.log').read_text(), (win / 'run.log').read_text()]
        if not a.map_exits:
            for log in logs:
                assert len(re.findall(r'm4a call \d+ m4aSongNumStart 62 ', log)) >= 2, 'callback was not executed'
        assert 'host64: image moved;' in logs[0]
        for path in (out / 'android-msg.log', win / 'msg.log'):
            text = path.read_text().upper()
            if not a.map_exits:
                assert 'A301596E0008' in text and 'A402596E0008' in text, 'wire callback ID is not canonical'
            assert 'A301' + struct.pack('<I', native).hex().upper() not in text
        if a.map_exits:
            for path in (out / 'android-state.log', win / 'state.log'):
                areas = {tuple(line.split()[4:6]) for line in path.read_text().splitlines()
                         if len(line.split()) >= 6 and line.split()[4] != '0'}
                assert len(areas) >= 2, 'no map change observed: ' + repr(areas)
        result = dict(apk_sha256=hashlib.sha256(a.apk.read_bytes()).hexdigest(),
                      windows_callback=hex(native), wire_callback='0x08006E59', frames=42000,
                      android_abi='arm64-v8a', forced_relocation=True, passed=True)
        result['map_exits'] = a.map_exits
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result))
    finally:
        shell('am force-stop ' + PACKAGE, check=False)
        shell('rm -f ' + FOLDER + '/shiningsource.test.env', check=False)
        for child in (peer, relay):
            if child and child.poll() is None: child.terminate(); child.wait(timeout=10)


if __name__ == '__main__': main()
