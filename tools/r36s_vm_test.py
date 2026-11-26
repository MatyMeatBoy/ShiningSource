#!/usr/bin/env python3
"""Prove a ShiningSource R36S package (Linux AArch64) runs, in QEMU: a 4-core Cortex-A35 "virt" machine.

    py -3.12 tools/r36s_vm_test.py ZIP --rom ROM --out DIR [--frames 3000] [--smoke-frames 1500] [--no-kms]
                                   [--reference tools/tests/ga36_hash_reference.txt] [--picture-reference HASH]

One boot runs every case in order and powers off. The guest is Debian's arm64 kernel (6.12) with the installer's
busybox initrd; the program runs inside the root of a dArkOS image (Debian 13 for RK3326 handhelds, mounted read
only; its bash, its glibc 2.41), and the headless cases a second time under Ubuntu 20.04's glibc 2.31 through
that system's own loader (the oldest glibc the package is built for). The package ZIP and the ROM are put into the
initrd; files come back through a virtio serial port into DIR/outbox.tar. There is no sound card (SDL's dummy
driver). With a virtio-gpu display (the default; --no-kms leaves it out) the launcher drives a real DRM device:
dumb buffers, mode setting and page flips, as on the console.

Cases:
  hash        ss2_r36s_headless, SS_FRAMES=N SS_HASH_ALL=1 over tools/tests/play_input.txt (glibc 2.31): every
              frame's hash must equal the PC reference (tools/ga36_hash_reference.py)
  flat-1      the same game with every frame drawn as the Flat canvas (SS_TEST_RENDER=320 SS_VIEW_H=240) on one
              thread (SS_THREADS=1): one hash over all pictures, and the stage times (SS_PROF)
  flat-4      the same on four threads: the same picture hash, the render time with every core
  flat-darkos the four-thread case under dArkOS's glibc
  launcher    the package's ShiningSource.sh (bash) with SS_QUIT and SS_FB_SHOT: the whole program (KMS or
              /dev/fb0, SDL offscreen, the controller code, pacing), last-run.log and the panel picture
  fbfile      ss2_r36s with a 640x480 XRGB8888 file as the panel (the copy to the panel, deterministic)

Times inside QEMU are emulated (TCG on this PC): compare the cases with each other, never with the console.
Exit status 0 when every check passed; DIR/result.json holds them. The initrd in DIR holds the ROM and is
deleted after the run (--keep-initrd keeps it).
"""
import argparse
import gzip
import io
import json
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ga36_fb_to_png import convert as fb_to_png  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
LAB = Path('D:/01Claude/Dynabrothers1+2/local/darkos-vm')
QEMU = Path('D:/01Claude/ReKHCoM/toolchain/qemu/runtime/qemu-system-aarch64.exe')
FOCAL = LAB / 'focal/lib/aarch64-linux-gnu'
FOCAL_LIBS = ('ld-linux-aarch64.so.1', 'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1')
GAME = 'shiningsource'
OUTBOX_PORT = 'r36s.outbox'
HASH_LINE = re.compile(r'^\s*frame\s+(\d+)\s+hash\s+([0-9A-Fa-f]{8})', re.M)
PICTURE_LINE = re.compile(r'picture: (\d+) frames (\d+) wide, hash ([0-9A-F]{8})')
PROF_LINE = re.compile(r'prof: ([^:]+): n (\d+)\s+mean ([\d.]+)\s+p50 ([\d.]+)\s+p99 ([\d.]+)\s+max ([\d.]+)')
# Modules from Debian's kernel package: the disk, the dArkOS file system, the serial port, the display.
MODULE_NAMES = {'virtio', 'virtio_ring', 'virtio_mmio', 'virtio_blk', 'virtio_console', 'virtio_pci',
                'virtio_pci_modern_dev', 'virtio_pci_legacy_dev', 'virtio_dma_buf', 'btrfs', 'xor', 'xor-neon',
                'raid6_pq', 'xxhash', 'blake2b_generic', 'zstd_compress', 'zstd_decompress', 'zstd_common',
                'crc32c_generic', 'libcrc32c'}
MODULE_FOLDERS = ('kernel/drivers/gpu/drm/', 'kernel/drivers/video/', 'kernel/drivers/dma-buf/')


def cpio(out, path, mode, data=b''):
    name = path.encode() + b'\0'
    fields = (0, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    out.write(b'070701' + ''.join(f'{value:08x}' for value in fields).encode())
    out.write(name + bytes((-(110 + len(name))) % 4))
    out.write(data + bytes(-len(data) % 4))


class Tree:
    def __init__(self, out):
        self.out, self.dirs = out, set()

    def dir(self, name):
        for parent in [*reversed(Path(name).parents), Path(name)]:
            key = parent.as_posix()
            if key not in ('.', '') and key not in self.dirs:
                self.dirs.add(key)
                cpio(self.out, key, stat.S_IFDIR | 0o755)

    def file(self, name, data, mode=0o644):
        self.dir(Path(name).parent.as_posix())
        cpio(self.out, name, stat.S_IFREG | mode, data)


def deb_payload(file):
    package = file.read_bytes()
    pos = 8
    while pos < len(package):
        size = int(package[pos + 48:pos + 58])
        if package[pos:pos + 16].startswith(b'data.tar'):
            return io.BytesIO(package[pos + 60:pos + 60 + size])
        pos += 60 + size + size % 2
    raise ValueError(f'no data archive in {file}')


def add_modules(tree, deb):
    with tarfile.open(fileobj=deb_payload(deb)) as archive:
        for member in archive:
            name = Path(member.name)
            if 'modules' not in name.parts or not member.isfile():
                continue
            inside = Path(*name.parts[name.parts.index('modules') + 2:]).as_posix()
            if not (name.name.split('.ko')[0] in MODULE_NAMES or name.name.startswith('modules.')
                    or inside.startswith(MODULE_FOLDERS)):
                continue
            path = Path('usr/lib') / Path(*name.parts[name.parts.index('modules'):])
            tree.file(path.as_posix(), archive.extractfile(member).read())


INIT = r'''#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin
B=/bin/busybox
$B mount -t proc proc /proc
$B mount -t sysfs sysfs /sys
$B mount -t devtmpfs devtmpfs /dev
printf 'R36S_VM_BOOTED arch=%s kernel=%s cpus=%s\n' "$(uname -m)" "$(uname -r)" "$(grep -c ^processor /proc/cpuinfo)"
depmod -a 2>/dev/null
for m in virtio_mmio virtio_pci virtio_blk virtio_console btrfs; do modprobe $m 2>/dev/null; done
@KMS@
$B sleep 1
OUTBOX=
for i in 1 2 3 4 5 6 7 8 9 10; do
    for p in /sys/class/virtio-ports/*; do
        if [ "$(cat "$p/name" 2>/dev/null)" = @PORT@ ] && [ -e "/dev/${p##*/}" ]; then OUTBOX="/dev/${p##*/}"; fi
    done
    [ -n "$OUTBOX" ] && break
    $B sleep 1
done
echo "R36S_OUTBOX=${OUTBOX:-none}"
$B mkdir -p /disk
for part in /dev/vda[1-9]; do
    if $B mount -t btrfs -o ro,nologreplay "$part" /disk 2>/dev/null; then
        if [ -x /disk/bin/bash ] && [ -r /disk/etc/os-release ]; then echo "R36S_ROOT=$part"; break; fi
        $B umount /disk
    fi
done
if [ ! -x /disk/bin/bash ]; then
    echo R36S_NO_ROOT
    $B dmesg | $B tail -30
    $B poweroff -f
fi
$B mount -t tmpfs -o size=900m tmpfs /disk/run
$B mount --bind /dev /disk/dev
$B mount --bind /proc /disk/proc
$B mount --bind /sys /disk/sys
$B mkdir -p /disk/run/out
$B cp -a /lab /disk/run/lab
$B chroot /disk /bin/bash /run/lab/body.sh
if [ -n "$OUTBOX" ]; then $B tar -cf "$OUTBOX" -C /disk/run out && echo R36S_OUTBOX_WRITTEN; fi
$B sync
echo R36S_DONE
$B poweroff -f
'''

KMS = r'''modprobe virtio-gpu 2>/dev/null
for i in 1 2 3 4 5; do [ -e /dev/dri/card0 ] && break; $B sleep 1; done
{ ls -l /dev/dri /dev/fb0; cat /sys/class/drm/card*-*/modes /sys/class/drm/card*-*/status; } > /lab/kms-devices.txt 2>&1'''

BODY_HEAD = r'''#!/bin/bash
export PATH=/usr/bin:/usr/sbin:/bin:/sbin HOME=/run
cd /run/lab
cp kms-devices.txt /run/out/ 2>/dev/null
{ cat /etc/os-release; getconf GNU_LIBC_VERSION; uname -a; } > /run/out/system.txt 2>&1
FOCAL=/run/lab/focal
G=/run/lab/@GAME@
# case NAME COMMAND...: the command in a subshell, its output in /run/out/NAME.log, its time and status
case_run() {
    local name=$1; shift
    local start=$(date +%s.%N)
    # each case in a new folder: what one run leaves (shiningsource-language.txt) changes the next one's game
    mkdir -p /run/work/$name
    ( cd /run/work/$name && "$@" ) > /run/out/$name.log 2>&1
    local code=$?
    local end=$(date +%s.%N)
    echo "R36S_CASE $name exit=$code seconds=$(awk "BEGIN { print $end - $start }")"
    echo "$code" > /run/out/$name.exit
}
'''


def shell_quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def env_prefix(env):
    return ' '.join(shell_quote(f'{k}={v}') for k, v in env.items())


def body(args):
    common = {'SS_SKIP': '0', 'SS_NET': '', 'SS_INPUT_FILE': '/run/lab/input.txt', 'SDL_AUDIO_DRIVER': 'dummy'}
    focal = f'"$FOCAL/ld-linux-aarch64.so.1" --library-path "$FOCAL:$G/lib"'
    flat = {'SS_FRAMES': str(args.frames), 'SS_TEST_RENDER': '320', 'SS_VIEW': '320', 'SS_VIEW_H': '240',
            'SS_PROF': '1'}
    lines = [BODY_HEAD.replace('@GAME@', GAME), 'cd "$G"']

    def add(name, env, command):
        merged = {**common, **env}
        lines.append(f'case_run {name} env {env_prefix(merged)} {command}')

    for index, case in enumerate(args.case):
        env = dict(item.partition('=')[::2] for item in case.split(',') if item)
        if env.pop('BIN', '') == 'sdl':     # the whole program, its panel a file (SS_FBDEV), no pacing with SS_PICTURE=2
            lines.append(f'dd if=/dev/zero of=/run/fb-{index}.bin bs=1228800 count=1 2>/dev/null')
            add(f'extra-{index}', {'SS_THREADS': '4', 'SS_FBDEV': f'/run/fb-{index}.bin', 'SS_FBDEV_MODE': '640x480x32',
                                   'SS_ASSETS': f'/run/lab/{GAME}/', **env}, '"$G/ss2_r36s" --game "$G/roms/rom.gba"')
            continue
        add(f'extra-{index}', {'SS_THREADS': '4', **env}, '"$G/ss2_r36s_headless" "$G/roms/rom.gba" "$G/ss2_game.macho"')
    if args.only_cases:
        return '\n'.join(lines) + '\n'
    add('hash', {'SS_FRAMES': str(args.frames), 'SS_HASH_ALL': '1'}, f'{focal} "$G/ss2_r36s_headless" "$G/roms/rom.gba" "$G/ss2_game.macho"')
    add('flat-1', {**flat, 'SS_THREADS': '1'}, f'{focal} "$G/ss2_r36s_headless" "$G/roms/rom.gba" "$G/ss2_game.macho"')
    add('flat-4', {**flat, 'SS_THREADS': '4'}, f'{focal} "$G/ss2_r36s_headless" "$G/roms/rom.gba" "$G/ss2_game.macho"')
    add('flat-darkos', {**flat, 'SS_THREADS': '4'}, '"$G/ss2_r36s_headless" "$G/roms/rom.gba" "$G/ss2_game.macho"')
    shot = max(1, args.smoke_frames // 2)
    # SS_FRAMESKIP=0: every frame is shown, so the shot is game frame `shot` whatever the emulation's speed
    launcher = {'SS_QUIT': str(args.smoke_frames), 'SS_FB_SHOT': str(shot), 'SS_FRAMESKIP': '0', 'SS_KMS_ANY': '1',
                'SS_FRAMELOG':
                '/run/out/launcher-frames.csv'}
    lines.append('mkdir -p /run/lab/ports && cp -a "$G" /run/lab/ports/ && cp /run/lab/ShiningSource.sh /run/lab/ports/')
    add('launcher', launcher, 'bash /run/lab/ports/ShiningSource.sh')
    lines.append(f'cp /run/lab/ports/{GAME}/user/last-run.log /run/out/last-run.log 2>/dev/null')
    lines.append(f'cp /run/lab/ports/{GAME}/user/shiningsource_fb.raw /run/out/launcher-fb.raw 2>/dev/null')
    if args.kms:
        # SDL3's own KMSDRM driver (SS_FBDEV empty): needs the firmware's EGL/GBM; information only
        add('sdl-kmsdrm', {'SS_FBDEV': '', 'SS_QUIT': '600', 'SS_WINDOW_SHOT': '400'},
            'bash /run/lab/ports/ShiningSource.sh')
        lines.append(f'cp /run/lab/ports/{GAME}/user/last-run.log /run/out/sdl-kmsdrm-last-run.log 2>/dev/null')
        lines.append(f'cp /run/lab/ports/{GAME}/user/ss_window.bmp /run/out/sdl-kmsdrm.bmp 2>/dev/null')
    lines.append('dd if=/dev/zero of=/run/fb.bin bs=1228800 count=1 2>/dev/null')
    add('fbfile', {'SS_FBDEV': '/run/fb.bin', 'SS_FBDEV_MODE': '640x480x32', 'SS_CANVAS': '320x240',
                   'SS_ASSETS': f'/run/lab/{GAME}/', 'SS_QUIT': str(args.smoke_frames), 'SS_FB_SHOT': str(shot),
                   'SS_FRAMESKIP': '0', 'SS_STATS': '1'},
        '"$G/ss2_r36s" --game "$G/roms/rom.gba"')
    lines.append('cp /run/work/fbfile/shiningsource_fb.raw /run/out/fbfile-fb.raw 2>/dev/null; cp /run/fb.bin /run/out/fbfile-panel.raw')
    return '\n'.join(lines) + '\n'


def build_initrd(args, path):
    shutil.copyfile(LAB / 'installer-initrd.gz', path)
    package = zipfile.ZipFile(args.zip)
    with gzip.open(path, 'ab', compresslevel=1) as out:
        tree = Tree(out)
        add_modules(tree, LAB / 'kernel-modules.deb')
        init = INIT.replace('@PORT@', OUTBOX_PORT).replace('@KMS@', KMS if args.kms else ': no display')
        tree.file('r36s-init', init.encode(), 0o755)
        script = body(args)
        tree.file('lab/body.sh', script.encode(), 0o755)
        for entry in package.infolist():
            if entry.is_dir():
                continue
            mode = (entry.external_attr >> 16) & 0o777 or 0o644
            tree.file(f'lab/{entry.filename}', package.read(entry), mode)
        tree.file(f'lab/{GAME}/roms/rom.gba', args.rom.read_bytes())
        tree.file('lab/input.txt', args.input.read_bytes().replace(b'\r\n', b'\n'))
        for name in FOCAL_LIBS:
            tree.file(f'lab/focal/{name}', (FOCAL / name).read_bytes(), 0o755)
        cpio(out, 'TRAILER!!!', 0)
    (args.out / 'body.sh').write_text(script, newline='\n')


def run_vm(args, initrd):
    outbox = args.out / 'outbox.tar'
    command = [str(args.qemu), '-machine', 'virt', '-cpu', 'cortex-a35', '-smp', '1' if args.icount else '4',
               '-m', '1536']
    # --icount: one vCPU whose clock advances 1 ns per executed instruction, so SS_PROF's microseconds read as
    # thousands of guest instructions (no caches or memory timing: an instruction count, not a speed)
    command += (['-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off,align=off'] if args.icount
                else ['-accel', 'tcg,thread=multi,tb-size=256'])
    command += [
               '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-nic', 'none', '-no-reboot',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-device', 'virtio-blk-device,drive=os',
               '-device', 'virtio-serial-device',
               '-chardev', f'file,id=outbox,path={outbox.resolve().as_posix()}',
               '-device', f'virtserialport,chardev=outbox,name={OUTBOX_PORT}']
    if args.kms:
        command += ['-device', 'virtio-gpu-pci,xres=640,yres=480']
    command += ['-kernel', str(LAB / 'linux-arm64'), '-initrd', str(initrd),
                '-append', 'console=ttyAMA0 rootfstype=ramfs rdinit=/r36s-init quiet panic=1']
    (args.out / 'qemu-command.txt').write_text(subprocess.list2cmdline(command) + '\n')
    flags = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
    started, timed_out = time.time(), False
    with (args.out / 'serial.log').open('wb') as log:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=flags)
        try:
            proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            proc.wait()
    return timed_out, time.time() - started


class Checks:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail=''):
        self.items.append({'check': name, 'ok': bool(ok), 'detail': detail})
        print(f'  [{"PASS" if ok else "FAIL"}] {name}' + (f': {detail}' if detail else ''))
        return ok

    @property
    def ok(self):
        return all(item['ok'] for item in self.items)


def text_of(path):
    return path.read_text(errors='replace') if path.exists() else ''


def prof(text):
    return {m[1]: {'n': int(m[2]), 'mean_us': float(m[3]), 'p50_us': float(m[4]), 'p99_us': float(m[5]),
                   'max_us': float(m[6])} for m in PROF_LINE.finditer(text)}


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('zip', type=Path)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=3000)
    parser.add_argument('--smoke-frames', type=int, default=1500)
    parser.add_argument('--input', type=Path, default=ROOT / 'tools/tests/play_input.txt')
    parser.add_argument('--reference', type=Path, default=ROOT / 'tools/tests/ga36_hash_reference.txt')
    parser.add_argument('--picture-reference', help='the PC\'s Flat picture hash for the same frames')
    parser.add_argument('--image', type=Path, default=LAB / 'dArkOS_RG351MP_trixie_11182026.img')
    parser.add_argument('--qemu', type=Path, default=QEMU)
    parser.add_argument('--no-kms', dest='kms', action='store_false')
    parser.add_argument('--timeout', type=int, default=5400)
    parser.add_argument('--keep-initrd', action='store_true')
    parser.add_argument('--case', action='append', default=[], metavar='VAR=V,VAR=V',
                        help='also run ss2_r36s_headless (ss2_r36s with BIN=sdl) with these variables (dArkOS glibc), '
                             'output extra-N.log')
    parser.add_argument('--only-cases', action='store_true', help='run only the --case runs (no checks)')
    parser.add_argument('--icount', action='store_true', help='one vCPU, -icount shift=0 (instruction counts)')
    args = parser.parse_args()
    for path in (args.zip, args.rom, args.input, args.image, args.qemu):
        if not path.is_file():
            sys.exit(f'r36s_vm_test: not found: {path}')
    if args.out.exists() and any(args.out.iterdir()):
        sys.exit(f'r36s_vm_test: {args.out} is not empty')
    args.out.mkdir(parents=True, exist_ok=True)
    initrd = args.out / 'initrd.gz'
    build_initrd(args, initrd)
    print(f'booting QEMU (4 x Cortex-A35, timeout {args.timeout} s) ...', flush=True)
    timed_out, wall = run_vm(args, initrd)
    if not args.keep_initrd:
        initrd.unlink(missing_ok=True)
    serial = text_of(args.out / 'serial.log').replace('\r', '')
    checks = Checks()
    checks.add('VM finished before the timeout', not timed_out, f'{wall:.0f} s')
    checks.add('guest booted, found the dArkOS root', 'R36S_ROOT=' in serial, (re.findall(r'R36S_\w+=?\S*', serial) or ['?'])[0])
    out = args.out / 'fetched'
    tar = args.out / 'outbox.tar'
    if checks.add('results copied out', tar.exists() and tar.stat().st_size, f'{tar}'):
        with tarfile.open(tar) as archive:
            archive.extractall(out, filter='data')
    out = out / 'out'
    cases = dict(re.findall(r'^R36S_CASE (\S+) (.*)$', serial, re.M))
    for name, line in cases.items():
        print(f'  [info] {name}: {line}')
    report = {'wall_seconds': round(wall, 1), 'cases': cases}
    for index, case in enumerate(args.case):
        text = text_of(out / f'extra-{index}.log')
        found = [*PICTURE_LINE.findall(text), *[m for m in re.findall(r'^frame\s+\d+\s+hash [0-9A-F]{8}', text, re.M)][-1:]]
        print(f'  [case] {case}: {found}')
        for stage, value in prof(text).items():
            print(f'      {stage}: mean {value["mean_us"]:.1f}, p50 {value["p50_us"]:.1f}, p99 {value["p99_us"]:.1f}')
    if args.only_cases:
        return

    hashes = {int(f): h.upper() for f, h in HASH_LINE.findall(text_of(out / 'hash.log'))}
    reference = {int(f): h.upper() for f, h in HASH_LINE.findall(args.reference.read_text())}
    first = next((f for f in sorted(set(hashes) | set(reference)) if hashes.get(f) != reference.get(f)), None)
    checks.add(f'hash: {args.frames} frames equal the PC reference (glibc 2.31)', hashes and first is None,
               f'{len(hashes)} frames, last {hashes.get(args.frames)}' if first is None else
               f'first difference at frame {first}: VM {hashes.get(first)} PC {reference.get(first)}')
    pictures = {}
    for name in ('flat-1', 'flat-4', 'flat-darkos'):
        text = text_of(out / f'{name}.log')
        match = PICTURE_LINE.search(text)
        pictures[name] = match[3] if match else None
        report[name] = {'picture': pictures[name], 'prof': prof(text)}
    checks.add('Flat pictures identical on 1 and 4 threads and both glibcs', pictures['flat-1'] and
               len(set(pictures.values())) == 1, json.dumps(pictures))
    if args.picture_reference:
        checks.add('Flat pictures equal the PC', pictures['flat-1'] == args.picture_reference.upper(),
                   f'VM {pictures["flat-1"]} PC {args.picture_reference.upper()}')
    for name in ('flat-1', 'flat-4'):
        stages = report[name]['prof']
        print(f'  [info] {name} (emulated): ' + ', '.join(f'{k} {v["mean_us"] / 1000:.2f} ms' for k, v in stages.items()))
    log = text_of(out / 'last-run.log')
    (args.out / 'last-run.log').write_text(log, newline='\n')
    checks.add('launcher: exit status 0', 'exit status: 0' in log, (re.findall(r'exit status: \d+', log) or ['none'])[-1])
    if args.kms:
        kms = re.search(r'^kms: /dev/dri/\S+, .*$', log, re.M)
        checks.add('launcher: picture through KMS (3 buffers, page flips)', kms, kms[0] if kms else 'no "kms:" mode line')
    pacing = re.findall(r'^video: .*$', log, re.M)
    print('  [info] ' + '\n  [info] '.join(pacing or ['no video: line']))
    for line in re.findall(r'^(?:stats|host64|kms|fbdev): .*$', log, re.M):
        print(f'  [log] {line}')
    for name, raw in (('launcher', out / 'launcher-fb.raw'), ('fbfile', out / 'fbfile-fb.raw')):
        if raw.exists():
            info = fb_to_png(raw, args.out / f'{name}-fb.png')
            checks.add(f'{name}: panel picture not black', info['non_black'] > 10000,
                       f"{info['non_black']} pixels lit, {args.out / (name + '-fb.png')}")
        else:
            checks.add(f'{name}: panel picture written (SS_FB_SHOT)', False, 'no shiningsource_fb.raw')
    checks.add('fbfile: exit 0', text_of(out / 'fbfile.exit').strip() == '0', text_of(out / 'fbfile.exit').strip())
    sdl = text_of(out / 'sdl-kmsdrm-last-run.log')
    if sdl:
        print('  [info] SDL3 KMSDRM fallback: ' + '; '.join(re.findall(r'^(?:video|window|SDL_Init|exit status).*$', sdl, re.M)))
    report['checks'] = checks.items
    (args.out / 'result.json').write_text(json.dumps(report, indent=2))
    if not checks.ok:
        sys.exit(f'r36s_vm_test: FAILED; see {args.out}')
    print(f'r36s_vm_test: PASSED; results in {args.out}')


if __name__ == '__main__':
    main()
