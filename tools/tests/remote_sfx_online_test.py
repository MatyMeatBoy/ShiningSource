"""Compare experimental combat cues on/off in two disposable Windows clients.

Supply your own European ROM. Saves/logs are confined to --out. This exercises
one scripted starting encounter, not every weapon or a physical audio device.
The compiled source-branch tests cover the other sound gates and deduplication.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rom', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--exe', type=Path, default=ROOT / 'build/win64/ss2/ss2_win64.exe')
    p.add_argument('--port', type=int, default=47963)
    args = p.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    rom = args.rom.resolve()
    base = [(int(f), int(b, 16)) for f, b in re.findall(
        r'(\d+):([0-9A-Fa-f]+)', (ROOT / 'tools/tests/net_outage_input.txt').read_text())
        if int(f) < 23400]
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    results = []
    for enabled in (0, 1):
        case = args.out / ('on' if enabled else 'off')
        case.mkdir(exist_ok=True)
        children, files = [], []
        room = 'remote-sfx-%d-%d' % (enabled, time.time_ns())
        with (case / 'relay.log').open('w') as relay_log:
            relay = subprocess.Popen([str(ROOT / 'build/relay/ss_relay.exe'), str(args.port)],
                                     stdout=relay_log, stderr=subprocess.STDOUT, creationflags=flags)
            try:
                time.sleep(.4)
                for who in (0, 1):
                    folder = case / str(who)
                    folder.mkdir(exist_ok=True)
                    events = base + [(23400, 0)]
                    for at in (23410, 23440, 23470):
                        events.extend(((at, 0x200), (at + 6, 0)))
                    for at in range(23500, 27000, 40):
                        events.extend(((at, 1), (at + 6, 0)))
                    script = folder / 'input.txt'
                    script.write_text(','.join('%d:%04X' % v for v in sorted(events)))
                    env = {k: v for k, v in os.environ.items() if not k.startswith('SS_')}
                    env.update(SS_NET='127.0.0.1:%d' % args.port, SS_NET_ROOM=room,
                               SS_NET_DELAY='4', SS_SKIP='0', SS_NET_PLAYERS='2',
                               SS_NET_LOG='net.log', SS_SCENE_LOG='scene.log', SS_M4A_LOG='1',
                               SS_FRAMES='27000', SS_PACE='1', SS_PACE_FAST='27000:12',
                               SS_INPUT_FILE=str(script), SS_REMOTE_SFX=str(enabled),
                               SS_INPUT_HUNT='23500', SS_INPUT_HUNT_UNTIL='26300')
                    log = (folder / 'run.log').open('w')
                    files.append(log)
                    children.append(subprocess.Popen([str(args.exe.resolve()), str(rom)], cwd=folder,
                        env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=flags))
                    time.sleep(.25)
                for child in children:
                    assert child.wait(timeout=180) == 0
            finally:
                for child in children:
                    if child.poll() is None:
                        child.terminate()
                        child.wait(timeout=10)
                relay.terminate()
                relay.wait(timeout=10)
                for log in files:
                    log.close()
        counts, blocks = [], []
        for who in (0, 1):
            folder = case / str(who)
            log = (folder / 'run.log').read_text()
            assert 'frame 27000' in log and 'crash:' not in log
            counts.append(dict(collections.Counter(int(song, 16) for frame, song in re.findall(
                r'm4a call (\d+) m4aSongNumStart ([0-9A-F]+)', log)
                if 23400 <= int(frame) < 24000)))
            blocks.append({int(a): (b, c) for a, b, c in
                           (row.split() for row in (folder / 'net.log').read_text().splitlines())})
        common = blocks[0].keys() & blocks[1].keys()
        assert len(common) > 1000 and all(blocks[0][f] == blocks[1][f] for f in common)
        results.append(dict(enabled=enabled, shared_labels=len(common), block_mismatches=0,
                            cue_counts=counts))
    # This starting encounter selects the weapon whose presentation cue is0x14.
    # Require a positive control; a lobby-only run with no attacks must not pass.
    before = sum(peer.get(0x14, 0) for peer in results[0]['cue_counts'])
    after = sum(peer.get(0x14, 0) for peer in results[1]['cue_counts'])
    receipt = dict(runs=results, weapon_cue='0x14', disabled_calls=before, enabled_calls=after)
    (args.out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    assert before > 0 and after > before, receipt
    print('PASS: weapon cue0x14 calls off=%d on=%d; shared blocks match in both sessions' % (before, after))


if __name__ == '__main__':
    main()
