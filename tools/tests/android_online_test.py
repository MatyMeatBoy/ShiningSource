#!/usr/bin/env python3
"""Local relay interop: shipped x32 Android image, relocated headless host, Windows peer.

Run android_memory_test --block-image first; pass its compiled host. This tests
guest network determinism, not the Android UI/background multiplayer experience.
Requires the supported user ROM, a dedicated emulator and an existing Windows build.
"""
import argparse
import os
from pathlib import Path
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    args = parser.parse_args()
    out = ROOT / 'build/android/088-online'
    win = out / 'windows'
    win.mkdir(parents=True, exist_ok=True)
    remote = '/data/local/tmp/ss-online-regression'
    base = [args.adb, '-s', args.serial]

    def adb(*command):
        return subprocess.run([*base, *command], capture_output=True, text=True, check=True, timeout=180)

    with zipfile.ZipFile(args.apk) as archive:
        (out / 'ss2_game.elf').write_bytes(archive.read('assets/ss2_game.elf'))
    adb('shell', 'mkdir -p ' + remote)
    for source, name in ((args.host, 'host'), (out / 'ss2_game.elf', 'ss2_game.elf'),
                         (args.rom, 'rom.gba'), (ROOT / 'tools/tests/net_outage_input.txt', 'input.txt')):
        adb('push', str(source), remote + '/' + name)
    room = 'android088-' + str(int(time.time()))
    env = dict(os.environ, SS_NET='127.0.0.1:47958', SS_NET_ROOM=room, SS_NET_DELAY='4',
               SS_NET_PLAYERS='2', SS_NET_LOG='net.log', SS_FRAMES='24000', SS_PACE='1',
               SS_PACE_FAST='24000:12', SS_INPUT_FILE=str(ROOT / 'tools/tests/net_outage_input.txt'))
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    relay = peer = None
    with (out / 'relay.log').open('w') as relay_log, (win / 'run.log').open('w') as win_log:
        try:
            relay = subprocess.Popen([str(ROOT / 'build/relay/ss_relay.exe'), '47958'],
                                     stdout=relay_log, stderr=subprocess.STDOUT, creationflags=flags)
            time.sleep(0.5)
            peer = subprocess.Popen([str(ROOT / 'build/win64/ss2/ss2_win64.exe'), str(args.rom)],
                                    cwd=win, env=env, stdout=win_log, stderr=subprocess.STDOUT, creationflags=flags)
            command = ('cd ' + remote + ' && chmod 755 host && SS_IMAGE_MOVE=1 '
                       'SS_NET=10.0.2.2:47958 SS_NET_ROOM=' + room + ' SS_NET_DELAY=4 SS_NET_PLAYERS=2 '
                       'SS_NET_LOG=net.log SS_FRAMES=24000 SS_PACE=1 SS_PACE_FAST=24000:12 '
                       'SS_INPUT_FILE=input.txt ./host rom.gba >run.log 2>&1; status=$?; echo SS_TEST_EXIT:$status; exit $status')
            result = adb('shell', command)
            adb('pull', remote + '/run.log', str(out / 'android.log'))
            assert 'SS_TEST_EXIT:0' in result.stdout, result.stdout[-2000:]
            assert peer.wait(timeout=60) == 0
            adb('pull', remote + '/net.log', str(out / 'android-net.log'))
        finally:
            subprocess.run([*base, 'pull', remote + '/run.log', str(out / 'android.log')], capture_output=True, timeout=15)
            for child in (peer, relay):
                if child and child.poll() is None:
                    child.terminate()
                    child.wait(timeout=10)
    logs = []
    for path in (win / 'net.log', out / 'android-net.log'):
        rows = {int(fields[0]): (fields[1], fields[2]) for line in path.read_text().splitlines()
                if len(fields := line.split()) == 3}
        logs.append(rows)
    common = set(logs[0]) & set(logs[1])
    comparable = [label for label in common if logs[0][label][0] == logs[1][label][0]]
    assert len(comparable) >= 1000, 'insufficient shared multiplayer traffic'
    assert all(logs[0][label] == logs[1][label] for label in comparable), 'network block mismatch'
    for path in (win / 'run.log', out / 'android.log'):
        log = path.read_text()
        assert 'frame 24000' in log and 'crash:' not in log and 'FATAL' not in log, log[-2000:]
    print('PASS: relocated Android guest / Windows peer,', len(comparable), 'shared labels; zero block mismatches')


if __name__ == '__main__':
    main()
