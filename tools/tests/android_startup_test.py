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
    p.add_argument('--abi', choices=('armeabi-v7a', 'x86_64', 'arm64-v8a'), required=True)
    p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--launcher', action='store_true', help='press PLAY on MainActivity; works with the final APK')
    p.add_argument('--root-storage', action='store_true', help='dedicated root emulator: restore app ownership after injecting scoped-storage test files')
    p.add_argument('--test-keys', help='script the existing options menu by presented frame')
    p.add_argument('--log-remote-sfx', action='store_true', help='record live remote-sound option callback changes')
    p.add_argument('--screenshot-after', type=float, default=5, help='seconds after launcher start before capturing rendered output')
    p.add_argument('--move-image', action='store_true', help='force the linked image into another free range')
    p.add_argument('--memfd', action='store_true', help='force file-backed RX code mappings')
    p.add_argument('--no-audio-device', action='store_true', help='require gameplay with an unavailable SDL audio driver')
    p.add_argument('--window-frames', type=int, default=0, help='render this many frames instead of the headless 32000-frame hash run')
    p.add_argument('--lifecycle', action='store_true', help='exercise screen-off/background then wake/foreground during a rendered run')
    p.add_argument('--density-change', action='store_true', help='change emulator density during rendered gameplay; require the game process to survive')
    p.add_argument('--frames', type=int, default=32000, help='headless frame count (diagnostic shorter runs may set --expected-hash)')
    p.add_argument('--dump-frame', type=int, default=0, help='capture video/RAM/frame artifacts at this game frame')
    p.add_argument('--expected-hash', help='explicit final hash for a short diagnostic run')
    args = p.parse_args()
    if (args.no_audio_device or args.lifecycle or args.density_change) and args.window_frames < 600:
        p.error('audio/lifecycle checks require --window-frames of at least 600')
    args.out.mkdir(parents=True, exist_ok=True)

    def adb(*command, check=True):
        result = subprocess.run([args.adb, '-s', args.serial, *command], capture_output=True, text=True, timeout=60)
        if check and result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def shell(command, check=True):
        return adb('shell', command, check=check)

    def game_pid():
        # API21 toolbox has no pidof; ps still lists the full process name.
        for command in ('ps -A', 'ps'):
            for row in shell(command, check=False).splitlines():
                fields = row.split()
                if len(fields) > 1 and fields[-1] == PACKAGE + ':game':
                    return fields[1]
        return ''

    assert hashlib.sha1(args.rom.read_bytes()).hexdigest() == 'aa1288be9257337abdfe3034898cf6c7aa778fbc', 'unsupported test ROM'
    assert shell('getprop sys.boot_completed').strip() == '1', 'wait for emulator boot completion before testing'
    adb('install', '-r', '--abi', args.abi, str(args.apk))
    # On a fresh scoped-storage install Android must create its own app folder
    # before the shell can populate the disposable test data.
    shell('am start -W -n ' + PACKAGE + '/.MainActivity')
    shell('mkdir -p ' + FOLDER)
    adb('push', str(args.rom), FOLDER + '/rom.gba')
    adb('push', str(args.input), FOLDER + '/play_input.txt')
    env = args.out / 'shiningsource.test.env'
    settings = 'SS_SKIP=0\nSS_FRAMES=%d\nSS_INPUT_FILE=play_input.txt\nSS_HASH_ALL=1\n' % args.frames
    if args.dump_frame:
        settings += 'SS_DUMP=%d\n' % args.dump_frame
    if args.window_frames:
        settings = 'SS_SKIP=0\nSS_FRAMES=0\nSS_QUIT=%d\nSS_INPUT_FILE=play_input.txt\nSS_STATS=1\n' % args.window_frames
    if args.move_image:
        settings += 'SS_IMAGE_MOVE=1\n'
    if args.memfd:
        settings += 'SS_EXEC_MEMFD=1\n'
    if args.no_audio_device:
        settings += 'SDL_AUDIO_DRIVER=shining-no-such-driver\n'
    if args.test_keys:
        settings += 'SS_TEST_KEYS=' + args.test_keys + '\n'
    if args.log_remote_sfx:
        settings += 'SS_REMOTE_SFX_LOG=1\n'
    # Keep canonical offline tests isolated even when the online checkbox is
    # outside the small emulator's visible scroll area.
    settings += 'SS_NET=\n'
    env.write_text(settings, encoding='utf-8')
    adb('push', str(env), FOLDER + '/shiningsource.test.env')
    if args.root_storage:
        assert args.serial.startswith('emulator-') and shell('id -u').strip() == '0'
        package_info = shell('dumpsys package ' + PACKAGE)
        uid = int(re.search(r'userId=(\d+)', package_info)[1])
        shell('chown -R %d:1078 /data/media/0/Android/data/%s' % (uid, PACKAGE))
        shell('restorecon -RF /data/media/0/Android/data/' + PACKAGE)
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
            lifecycle_done = False
            density_done = False
            screenshot_done = False
            while time.monotonic() - start < (600 if args.abi == 'armeabi-v7a' else 180):
                log = shell('cat ' + FOLDER + '/shiningsource.log', check=False)
                native_ready = 'host64: image loaded' in log or 'android: lifecycle watch installed' in log
                if args.lifecycle and not lifecycle_done and native_ready and time.monotonic() - start > 3:
                    shell('input keyevent 223')
                    time.sleep(1)
                    shell('input keyevent 224')
                    shell('wm dismiss-keyguard', check=False)
                    lifecycle_done = True
                if args.density_change and not density_done and native_ready and time.monotonic() - start > 3:
                    pid = game_pid()
                    density = int(re.findall(r'\d+', shell('wm density'))[-1])
                    shell('wm density %d' % (density + 20))
                    time.sleep(1)
                    assert pid and game_pid() == pid, 'density change restarted the game process'
                    density_done = True
                if args.window_frames and not screenshot_done and native_ready and time.monotonic() - start > args.screenshot_after:
                    shot = subprocess.run([args.adb, '-s', args.serial, 'exec-out', 'screencap', '-p'], capture_output=True, timeout=60, check=True)
                    (args.out / ('screen-%d.png' % (run + 1))).write_bytes(shot.stdout)
                    screenshot_done = True
                complete = 'host64: exit code 0' in log if args.window_frames else 'audio:' in log
                if complete or 'host64: exit code ' in log or 'FATAL' in log or 'memory map ' in log or 'crash:' in log or 'startup failure:' in log:
                    break
                time.sleep(1)
            (args.out / ('run-%d.log' % (run + 1))).write_text(log, encoding='utf-8')
            pages = re.search(r'(?:rebased GBA memory; page size |pointer 4; page )(\d+)', log)
            assert pages, 'missing native page-size diagnostic'
            info['page_size'] = pages[1]  # sysconf also works on Android 5, which has no getconf utility
            if args.window_frames:
                assert 'host64: exit code 0' in log and 'stats:' in log, 'rendered run did not complete; inspect saved log'
            else:
                expected = args.expected_hash or (EXPECTED_HASH if args.frames == 32000 else None)
                assert re.search(r'frame\s+%d\s+hash ' % args.frames + (expected or r'[0-9A-F]+'), log), 'gameplay hash failed; inspect saved log'
                if args.frames == 32000:
                    assert EXPECTED_AUDIO in log, 'audio regression; inspect saved log'
            assert 'heap: allocate' not in log and 'FATAL' not in log, 'runtime error; inspect saved log'
            if args.move_image:
                assert 'host64: image moved;' in log, 'image movement was not exercised'
            if args.memfd:
                assert 'executable code: memory-file RX mapping' in log, 'file-backed code was not exercised'
            if args.no_audio_device:
                assert 'android: audio unavailable; continuing silently:' in log, 'audio failure was not exercised'
            if args.lifecycle:
                assert 'android: lifecycle background;' in log and 'android: lifecycle foreground;' in log, 'both lifecycle transitions required'
            if args.density_change:
                assert density_done, 'density change was not exercised'
            final_hash = re.search(r'frame\s+%d\s+hash\s+([0-9A-F]+)' % args.frames, log)
            final_audio = re.search(r'audio: [^\r\n]+', log)
            info['runs'].append({'run': run + 1, 'hash': None if args.window_frames else final_hash[1],
                                 'audio': final_audio[0] if final_audio else None, 'window_frames': args.window_frames,
                                 'seconds': round(time.monotonic() - start, 2)})
            print('%s run %d passed' % (args.abi, run + 1), flush=True)
    finally:
        if args.dump_frame:
            for name in ('ss_video_dump.bin', 'ss_ram_dump.bin', 'ss_frame.raw'):
                adb('pull', FOLDER + '/' + name, str(args.out / name), check=False)
        shell('am force-stop ' + PACKAGE)
        if args.density_change:
            shell('wm density reset', check=False)
        shell('rm -f ' + FOLDER + '/shiningsource.test.env')
        (args.out / 'results.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
