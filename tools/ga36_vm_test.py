#!/usr/bin/env python3
"""Prove a ShiningSource GA36 package (Linux ARMv7 hard-float, 640x480 RGB565 panel) starts and runs, in QEMU.

The package ZIP is unpacked into a Debian armhf netboot initrd (kernel 6.1 armmp, busybox userland), the ROM is
added next to it, and qemu-system-arm boots a Cortex-A7 "virt" machine whose init script runs the package and
powers off. Files come back through a virtio serial port written into DIR/outbox.tar (a tar of the guest's
/run/out); without that port they come base64-encoded over the serial console between GA36_TAR_BEGIN/END.
Everything the guest prints is in DIR/serial.log. Inside the VM there is no display or sound: the panel is a
regular 614400-byte file given to the program as SS_FBDEV, SDL_VIDEO_DRIVER=offscreen, SDL_AUDIO_DRIVER=dummy.

    py -3.12 tools/ga36_vm_test.py smoke ZIP --rom ROM --out DIR [--frames 600] [--input FILE]
        runs the package's launcher ("ShiningSource GA36.sh") until SS_QUIT; checks exit code 0, an "fbdev:" line
        in the program's output or logs, and a framebuffer that is not all black; writes DIR/panel.png (and
        DIR/shiningsource_fb.png when the program wrote shiningsource_fb.raw for SS_FB_SHOT).
    py -3.12 tools/ga36_vm_test.py hash ZIP --rom ROM --frames N [--input FILE] --out DIR [--compare REF.txt]
        runs ss2_ga36_headless (else ss2_ga36) through the bundled loader with SS_FRAMES=N SS_HASH_ALL=1;
        saves DIR/hashes.txt ("frame N hash H" lines) and reports the first frame that differs from REF.txt
        (any text holding such lines, e.g. a desktop build's stderr).
    py -3.12 tools/ga36_vm_test.py diag ZIP --rom ROM --out DIR
        runs "ShiningSource GA36 - Diagnostico.sh" (4 vCPUs, 1 GiB, 1 h by default) and copies every
        diagnostico-NNN folder it made to DIR/fetched/out/diagnostico-NNN.
    py -3.12 tools/ga36_vm_test.py bench ZIP --rom ROM --frames N --out DIR
        one vCPU with -icount shift=0: the guest clock advances 1 ns per executed guest instruction, so the
        per-frame stage times in SS_FRAMELOG's frames.csv read as GUEST INSTRUCTIONS (ms = millions), not time.
        No caches, memory bandwidth or NEON latency are modelled: compare builds with it, do not read speed.
    py -3.12 tools/ga36_vm_test.py ra ZIP --rom ROM --input tools/tests/play_input.txt --out DIR [--frames 3600]
        RetroAchievements on the console with a pretend site (SS_RA_TEST_SERVER, tools/ra_fixture.py: no network,
        no real account; the VM has no network card at all). The card holds ReKHCoM's folder with a pretend
        sign-in (lab/rekhcomga36/retroachievements.auth) and the package's own folder none. Four runs of the
        launcher: 0) as shipped (its 52 achievements, the real HTTPS path failing without a network; a test
        notice at frame 120: the panel at the end, frame 200, is DIR/notice.png); 1) offline: the test achievement unlocks,
        is recorded and queued for the account; 2) online: the queue is sent (awardachievement) and emptied;
        3) Rebel (SS_CHEATS=hp): nothing is sent. Checks each run's retroachievements_estado.txt.
    py -3.12 tools/ga36_vm_test.py no-rom-selftest ZIP --out DIR
        without a ROM: lists the binary's libraries through the bundled loader (all must resolve inside
        libarmhf/), then starts it on a ROM path that does not exist and expects a clean exit with its
        "cannot open the ROM" message. Proves the loader, the glibc bundle and the SDL linkage.

Common options: --timeout seconds for the whole VM (each mode has a default), --env KEY=VALUE (repeatable) adds a
variable for the program, --rom-name (default rom.gba, the name under <package>/roms/), --qemu/--kernel/--initrd.
QEMU runs at below-normal priority; on timeout only the QEMU process this tool started is killed. Exit status 0
means every check passed; DIR/result.json holds the checks. The initrd built in DIR holds the ROM and is deleted
after the run (--keep-initrd keeps it).
"""
import argparse
import base64
import csv
import gzip
import io
import json
import re
import shutil
import stat
import statistics
import struct
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ga36_fb_to_png import convert as fb_to_png  # noqa: E402

REKHCOM = Path('D:/01Claude/ReKHCoM')
NETBOOT = REKHCOM / 'pruebas/ga36-armhf-20261122/linux-netboot'
QEMU = REKHCOM / 'toolchain/qemu/runtime/qemu-system-arm.exe'
LAUNCHER = 'ShiningSource GA36.sh'
DIAGNOSTIC = 'ShiningSource GA36 - Diagnostico.sh'
LOADER = 'libarmhf/ld-linux-armhf.so.3'
PANEL_BYTES = 640 * 480 * 2
OUTBOX_PORT = 'ga36.outbox'
HASH_LINE = re.compile(r'^\s*frame\s+(\d+)\s+hash\s+([0-9A-Fa-f]{8})', re.M)
# Generous: with the host busy (other VMs, builds) the guest kernel alone took 2 minutes to reach PCI probing.
DEFAULT_TIMEOUT = {'smoke': 2400, 'hash': 2400, 'diag': 5400, 'bench': 5400, 'no-rom-selftest': 1500, 'ra': 7200}


# ---- initramfs (newc cpio; copied from Dynabrothers1+2/tools/r36s_vm/run.py) ----

def cpio(out, path, mode, data=b''):
    name = path.encode() + b'\0'
    fields = (0, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    out.write(b'070701' + ''.join(f'{value:08x}' for value in fields).encode())
    out.write(name + bytes((-(110 + len(name))) % 4))
    out.write(data + bytes(-len(data) % 4))


class Tree:
    """Writes files into the cpio stream, creating each parent directory once."""

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

    def link(self, name, target):
        self.dir(Path(name).parent.as_posix())
        cpio(self.out, name, stat.S_IFLNK | 0o777, target.encode())


# ---- the package ----

class Package:
    def __init__(self, path):
        self.path = path
        self.zip = zipfile.ZipFile(path)
        names = self.zip.namelist()
        found = [n for n in names if re.search(r'(^|/)[^/]+/ss2_ga36$', n)]
        if not found:
            raise SystemExit(f'{path}: no <folder>/ss2_ga36 in the ZIP; is it a GA36 package?')
        self.binary = found[0]
        self.game_dir = found[0].rsplit('/', 2)[-2]               # shiningsourcega36
        self.prefix = found[0][:-len(self.game_dir + '/ss2_ga36')]  # '' or e.g. 'ports/'
        self.names = {n[len(self.prefix):] for n in names if n.startswith(self.prefix)}

    def has(self, name):
        return name in self.names

    def read(self, name):
        return self.zip.read(self.prefix + name)

    def add_to(self, tree, root='lab'):
        for entry in self.zip.infolist():
            if not entry.filename.startswith(self.prefix):
                continue
            name = f'{root}/{entry.filename[len(self.prefix):]}'.rstrip('/')
            if entry.is_dir():
                tree.dir(name)
                continue
            unix = entry.external_attr >> 16
            data = self.zip.read(entry)
            if stat.S_ISLNK(unix):
                tree.link(name, data.decode())
                continue
            mode = unix & 0o777
            if not mode:   # a ZIP made on Windows carries no permissions
                base = name.rsplit('/', 1)[-1]
                mode = 0o755 if base.endswith('.sh') or base.startswith(('ss2_ga36', 'ld-linux')) else 0o644
            tree.file(name, data, mode | 0o600)


def elf_summary(data):
    """ARM ELF facts of a binary: class, machine, hard-float flag, interpreter."""
    if data[:4] != b'\x7fELF':
        return {'elf': False}
    info = {'elf': True, 'class': 32 if data[4] == 1 else 64}
    if info['class'] != 32:
        info['machine'] = struct.unpack_from('<H', data, 18)[0]
        return info
    machine, = struct.unpack_from('<H', data, 18)
    phoff, = struct.unpack_from('<I', data, 28)
    flags, = struct.unpack_from('<I', data, 36)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    info.update(machine=machine, arm=machine == 40, hard_float=bool(flags & 0x400), eabi=flags >> 24)
    for i in range(phnum):
        kind, offset, _, _, size = struct.unpack_from('<IIIII', data, phoff + i * phentsize)
        if kind == 3:   # PT_INTERP
            info['interpreter'] = data[offset:offset + size].rstrip(b'\0').decode(errors='replace')
    return info


# ---- the guest's init script ----

PROLOGUE = r'''#!/bin/sh
export PATH=/usr/bin:/usr/sbin:/bin:/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /run/out /tmp
export HOME=/run
printf 'GA36_VM_BOOTED arch=%s kernel=%s cpus=%s mem_kb=%s\n' "$(uname -m)" "$(uname -r)" \
    "$(grep -c ^processor /proc/cpuinfo)" "$(grep MemTotal /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
modprobe virtio_mmio 2>/dev/null
modprobe virtio_console 2>/dev/null
OUTBOX=
for i in 1 2 3 4 5 6 7 8 9 10; do
    for p in /sys/class/virtio-ports/*; do
        if [ "$(cat "$p/name" 2>/dev/null)" = @PORT@ ] && [ -e "/dev/${p##*/}" ]; then OUTBOX="/dev/${p##*/}"; fi
    done
    [ -n "$OUTBOX" ] && break
    sleep 1
done
echo "GA36_OUTBOX=${OUTBOX:-serial}"
finish() {
    echo "GA36_RESULT=$1"
    if [ -n "$OUTBOX" ]; then
        tar -cf "$OUTBOX" -C /run out && echo GA36_OUTBOX_WRITTEN
    else
        echo GA36_TAR_BEGIN
        tar -cf - -C /run out | base64
        echo GA36_TAR_END
    fi
    sync
    echo GA36_DONE
    poweroff -f
    exit 0
}
show() {
    echo "GA36_LOG_BEGIN $1"
    tail -n "${2:-60}" "$1"
    echo "GA36_LOG_END $1"
}
'''.replace('@PORT@', OUTBOX_PORT)


def shell_quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def exports(env):
    return ''.join(f'export {shell_quote(f"{k}={v}")}\n' for k, v in env.items())


def program_env(args, extra):
    env = {'SDL_VIDEO_DRIVER': 'offscreen', 'SDL_AUDIO_DRIVER': 'dummy', 'SDL_RENDER_DRIVER': 'software'}
    env.update(extra)
    for item in args.env:
        key, _, value = item.partition('=')
        env[key] = value
    return env


def direct_command(pkg, binary, game_args):
    return (f'./{LOADER} --library-path libarmhf ./{binary} {game_args}')


def panel_bytes(args):
    """The panel file's size: 640x480 RGB565, or what --env SS_FBDEV_MODE=WxHxBPP says (the real GA36: 640x480x32)."""
    for item in args.env:
        key, _, value = item.partition('=')
        match = re.fullmatch(r'(\d+)x(\d+)(?:x(16|32))?', value) if key == 'SS_FBDEV_MODE' else None
        if match:
            return int(match[1]) * int(match[2]) * int(match[3] or 16) // 8
    return PANEL_BYTES


def body_smoke(args, pkg):
    kms = getattr(args, 'kms', False)
    extra = {'SS_FBDEV': '/dev/fb0' if kms else '/run/fb.bin', 'SS_QUIT': str(args.frames),
             'SS_FB_SHOT': str(max(1, args.frames - 10))}
    if kms:
        extra['SS_KMS_ANY'] = '1'
    if args.input:
        extra['SS_INPUT_FILE'] = '/lab/input.txt'
    env = program_env(args, extra)
    if args.direct:
        run = f'cd /lab/{pkg.game_dir} && {direct_command(pkg, "ss2_ga36", f"--game roms/{args.rom_name}")}'
    else:
        run = f'cd /lab && sh {shell_quote("./" + LAUNCHER)}'
    # --kms: a virtio-gpu display, so the program drives a real DRM device (page flips, mode setting)
    prepare = ('{ modprobe virtio_pci; modprobe -v virtio-gpu; sleep 2; ls -l /dev/dri /dev/fb0; ls /sys/class/drm; dmesg | grep -i -E \"virtio|drm|pci\" | tail -15; }'
               ' > /run/out/kms-devices.txt 2>&1'
               if kms else '')
    return f'''{prepare}
dd if=/dev/zero of=/run/fb.bin bs={panel_bytes(args)} count=1 2>/dev/null
( {exports(env).replace(chr(10), '; ')}{run} ) > /run/out/program.log 2>&1
code=$?
echo "GA36_EXIT=$code"
cp /run/fb.bin /run/out/panel.raw
for f in $(find /lab /run -maxdepth 3 -name shiningsource_fb.raw 2>/dev/null); do cp "$f" /run/out/; done
[ ! -d /lab/{pkg.game_dir}/user ] || cp -r /lab/{pkg.game_dir}/user /run/out/user
show /run/out/program.log
finish $code
'''


def body_hash(args, pkg):
    binary = 'ss2_ga36_headless' if pkg.has(f'{pkg.game_dir}/ss2_ga36_headless') else 'ss2_ga36'
    env = {'SS_FRAMES': str(args.frames), 'SS_HASH_ALL': '1'}
    if args.input:
        env['SS_INPUT_FILE'] = '/lab/input.txt'
    env = program_env(args, env)
    return f'''echo "GA36_BINARY={binary}"
( {exports(env).replace(chr(10), '; ')}cd /lab/{pkg.game_dir} && \
{direct_command(pkg, binary, f"--game roms/{args.rom_name}")} ) > /run/out/program.log 2>&1
code=$?
echo "GA36_EXIT=$code"
show /run/out/program.log 20
finish $code
'''


def body_diag(args, pkg):
    env = program_env(args, {'SS_FBDEV': '/run/fb.bin'})
    return f'''dd if=/dev/zero of=/run/fb.bin bs={PANEL_BYTES} count=1 2>/dev/null
( {exports(env).replace(chr(10), '; ')}cd /lab && sh {shell_quote("./" + DIAGNOSTIC)} ) > /run/out/program.log 2>&1
code=$?
echo "GA36_EXIT=$code"
for d in /lab/{pkg.game_dir}/diagnostico-*; do
    [ -d "$d" ] || continue
    echo "GA36_DIAG_DIR=${{d##*/}}"
    cp -r "$d" /run/out/
done
show /run/out/program.log
finish $code
'''


def body_bench(args, pkg):
    env = program_env(args, {'SS_FBDEV': '/run/fb.bin', 'SS_QUIT': str(args.frames),
                             'SS_FRAMELOG': '/run/out/frames.csv'})
    if args.direct:
        run = f'cd /lab/{pkg.game_dir} && {direct_command(pkg, "ss2_ga36", f"--game roms/{args.rom_name}")}'
    else:
        run = f'cd /lab && sh {shell_quote("./" + LAUNCHER)}'
    return f'''echo 0 > /proc/sys/kernel/randomize_va_space
dd if=/dev/zero of=/run/fb.bin bs={PANEL_BYTES} count=1 2>/dev/null
( {exports(env).replace(chr(10), '; ')}{run} ) > /run/out/program.log 2>&1
code=$?
echo "GA36_EXIT=$code"
show /run/out/program.log 30
finish $code
'''


def body_selftest(args, pkg):
    env = program_env(args, {})
    lines = [f'cd /lab/{pkg.game_dir} || finish 90']
    for binary in ('ss2_ga36', 'ss2_ga36_headless'):
        if not pkg.has(f'{pkg.game_dir}/{binary}'):
            continue
        lines += [
            f'./{LOADER} --library-path libarmhf --list ./{binary} > /run/out/{binary}.libs 2>&1',
            f'echo "GA36_LIST {binary}=$?"',
            f'show /run/out/{binary}.libs',
            f'( {exports(env).replace(chr(10), "; ")}'
            f'{direct_command(pkg, binary, "--game roms/no-such-rom.gba")} ) > /run/out/{binary}.log 2>&1',
            f'echo "GA36_NOROM {binary}=$?"',
            f'show /run/out/{binary}.log',
        ]
    lines += [
        # the launcher too, for information only: what a player without a ROM would see
        f'( {exports(env).replace(chr(10), "; ")}cd /lab && sh {shell_quote("./" + LAUNCHER)} ) '
        '> /run/out/launcher.log 2>&1',
        'echo "GA36_LAUNCHER=$?"',
        'show /run/out/launcher.log',
        'finish 0',
    ]
    return '\n'.join(lines) + '\n'


RA_PHASES = (
    # name, extra variables, a file removed before the run
    ('shipped', {'SS_RA_TEST_NOTICE': 'Test notice', 'SS_QUIT': '200'}, ''),
    ('offline', {'SS_RA_DIR': '/lab/ratest', 'SS_RA_TEST_SERVER': '/lab/raserver'}, ''),
    ('online', {'SS_RA_DIR': '/lab/ratest', 'SS_RA_TEST_SERVER': '/lab/raserver'}, '/lab/raserver/offline'),
    ('rebel', {'SS_RA_DIR': '/lab/ratest', 'SS_RA_TEST_SERVER': '/lab/raserver', 'SS_CHEATS': 'hp'},
     '/lab/raserver/requests.log'),
)


def body_ra(args, pkg):
    lines = ['dd if=/dev/zero of=/run/fb.bin bs=%d count=1 2>/dev/null' % panel_bytes(args)]
    for name, extra, remove in RA_PHASES:
        env = {'SS_FBDEV': '/run/fb.bin', 'SS_QUIT': str(args.frames), **extra}
        if name != 'shipped':       # (a scripted run never submits: the shipped run must be a player's)
            env['SS_INPUT_FILE'] = '/lab/input.txt'
        env = program_env(args, env)
        out = f'/run/out/{name}'
        lines += [
            f'mkdir -p {out}',
            f'rm -f {remove}' if remove else ':',
            f'( {exports(env).replace(chr(10), "; ")}cd /lab && sh {shell_quote("./" + LAUNCHER)} ) > {out}/program.log 2>&1',
            f'echo "GA36_RA_{name.upper()}=$?"',
            f'cp /lab/{pkg.game_dir}/user/last-run.log {out}/ 2>/dev/null',
            f'for f in /lab/raserver/requests.log /lab/{pkg.game_dir}/retroachievements_estado.txt; do [ -f "$f" ] && cp "$f" {out}/; done',
            f'[ -d /lab/ratest ] && cp -r /lab/ratest {out}/ratest',
            f'ls -la /lab/{pkg.game_dir} > {out}/game-folder.txt',
            f'cp /run/fb.bin {out}/panel.raw',
            f'show {out}/last-run.log 40',
        ]
    lines.append('finish 0')
    return '\n'.join(lines) + '\n'


BODIES = {'smoke': body_smoke, 'hash': body_hash, 'diag': body_diag, 'bench': body_bench,
          'no-rom-selftest': body_selftest, 'ra': body_ra}


# ---- running the VM ----

def build_initrd(args, pkg, path):
    shutil.copyfile(args.initrd, path)
    with gzip.open(path, 'ab', compresslevel=1) as out:
        tree = Tree(out)
        script = PROLOGUE + BODIES[args.mode](args, pkg)
        tree.file('ga36-init', script.encode(), 0o755)
        pkg.add_to(tree)
        tree.dir(f'lab/{pkg.game_dir}/user')
        if getattr(args, 'rom', None):
            tree.file(f'lab/{pkg.game_dir}/roms/{args.rom_name}', args.rom.read_bytes(), 0o644)
        if getattr(args, 'input', None):
            tree.file('lab/input.txt', args.input.read_bytes().replace(b'\r\n', b'\n'))
        if args.mode == 'ra':
            add_ra_fixture(tree, args.out / 'ra-fixture')
        cpio(out, 'TRAILER!!!', 0)
    (args.out / 'ga36-init.sh').write_text(script, newline='\n')


def add_ra_fixture(tree, staging):
    """The pretend site (offline at first), the test catalog in lab/ratest, and a pretend sign-in in ReKHCoM's
    folder beside the package's (the card of a player who linked ReKHCoM only)."""
    import ra_fixture
    shutil.rmtree(staging, ignore_errors=True)
    ra_fixture.write_server(staging / 'raserver', offline=True)
    ra_fixture.write_ra_dir(staging / 'ratest', shared=staging / 'rekhcomga36/retroachievements.auth')
    for path in sorted(p for p in staging.rglob('*') if p.is_file()):
        tree.file('lab/' + path.relative_to(staging).as_posix(), path.read_bytes())


def run_vm(args, initrd):
    outbox = args.out / 'outbox.tar'
    outbox.unlink(missing_ok=True)
    append = 'console=ttyAMA0 rootfstype=ramfs rdinit=/ga36-init quiet panic=1'
    command = [str(args.qemu), '-machine', 'virt', '-cpu', 'cortex-a7', '-smp', str(args.smp), '-m', str(args.mem)]
    if args.mode == 'bench':
        command += ['-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off,align=off',
                    '-rtc', 'base=2026-01-01T00:00:00,clock=vm', '-seed', '1']
        append += ' norandmaps'
    if getattr(args, 'kms', False):
        command[command.index('virt')] = 'virt,highmem=off'
        command += ['-device', 'virtio-gpu-pci,xres=640,yres=480']
    command += ['-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-nic', 'none', '-no-reboot',
                '-device', 'virtio-serial-device',
                '-chardev', f'file,id=outbox,path={outbox.resolve().as_posix()}',
                '-device', f'virtserialport,chardev=outbox,name={OUTBOX_PORT}',
                '-kernel', str(args.kernel), '-initrd', str(initrd), '-append', append]
    (args.out / 'qemu-command.txt').write_text(subprocess.list2cmdline(command) + '\n')
    flags = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
    started = time.time()
    timed_out = False
    with (args.out / 'serial.log').open('wb') as log:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=flags)
        try:
            proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()     # the QEMU this tool started, nothing else
            proc.wait()
    return proc.returncode, timed_out, time.time() - started


def fetch(args, serial):
    """Unpack the guest's /run/out into DIR/fetched; returns the folder or None."""
    data = None
    outbox = args.out / 'outbox.tar'
    if outbox.exists() and outbox.stat().st_size:
        data = outbox.read_bytes()
    elif 'GA36_TAR_BEGIN' in serial and 'GA36_TAR_END' in serial:
        text = serial.split('GA36_TAR_BEGIN', 1)[1].split('GA36_TAR_END', 1)[0]
        data = base64.b64decode(''.join(text.split()))
    if not data:
        return None
    target = args.out / 'fetched'
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(target, filter='data')
    return target / 'out'


def marker(serial, name):
    match = re.search(rf'^{re.escape(name)}=(.*)$', serial, re.M)
    return match.group(1).strip() if match else None


# ---- checks per mode ----

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


def read_text(path):
    return path.read_text(errors='replace') if path and path.exists() else ''


def logs_text(out):
    """The program's output and every log it left (user/*.log and the like)."""
    if not out:
        return ''
    parts = [read_text(p) for p in sorted(out.rglob('*')) if p.is_file() and p.suffix in ('.log', '.txt')]
    return '\n'.join(parts)


def check_smoke(args, out, serial, checks):
    checks.add('program exit code 0', marker(serial, 'GA36_EXIT') == '0', f'exit {marker(serial, "GA36_EXIT")}')
    text = logs_text(out)
    line = re.search(r'^.*fbdev:.*$', text, re.M)
    checks.add('"fbdev:" line logged', line, line.group(0).strip() if line else 'not in program.log or user logs')
    panel = out / 'panel.raw' if out else None
    if checks.add('panel file fetched', panel and panel.exists() and panel.stat().st_size == panel_bytes(args),
                  f'{panel.stat().st_size if panel and panel.exists() else 0} bytes'):
        info = fb_to_png(panel, args.out / 'panel.png')
        checks.add('panel not all black', info['non_black'], f"{info['non_black']} pixels lit; {args.out / 'panel.png'}")
    shot = out / 'shiningsource_fb.raw' if out else None
    if shot and shot.exists():
        try:
            info = fb_to_png(shot, args.out / 'shiningsource_fb.png')
            print(f"  [info] SS_FB_SHOT picture: {info['format']}, {info['non_black']} pixels lit; "
                  f"{args.out / 'shiningsource_fb.png'}")
        except ValueError as error:
            print(f'  [info] shiningsource_fb.raw not converted: {error}')
    else:
        print('  [info] no shiningsource_fb.raw (SS_FB_SHOT) was written')


def parse_hashes(text):
    return {int(frame): value.upper() for frame, value in HASH_LINE.findall(text)}


def check_hash(args, out, serial, checks):
    checks.add('program exit code 0', marker(serial, 'GA36_EXIT') == '0', f'exit {marker(serial, "GA36_EXIT")}')
    hashes = parse_hashes(read_text(out / 'program.log' if out else None))
    (args.out / 'hashes.txt').write_text(''.join(f'frame {f:5d}  hash {h}\n' for f, h in sorted(hashes.items())))
    checks.add(f'{args.frames} hash lines', len(hashes) >= args.frames and args.frames in hashes,
               f'{len(hashes)} lines in {args.out / "hashes.txt"}')
    if args.compare:
        reference = parse_hashes(args.compare.read_text(errors='replace'))
        common = sorted(set(hashes) | set(reference))
        first = next((f for f in common if hashes.get(f) != reference.get(f)), None)
        if first is None:
            checks.add('hashes equal the reference', True, f'{len(common)} frames identical')
        else:
            same = sum(1 for f in common if f < first)
            checks.add('hashes equal the reference', False,
                       f'first difference at frame {first}: VM {hashes.get(first, "missing")}, reference '
                       f'{reference.get(first, "missing")} ({same} frames equal before it)')


def check_diag(args, out, serial, checks):
    checks.add('diagnostic script exit code 0', marker(serial, 'GA36_EXIT') == '0',
               f'exit {marker(serial, "GA36_EXIT")}')
    folders = sorted(p for p in out.glob('diagnostico-*') if p.is_dir()) if out else []
    files = [p for d in folders for p in d.rglob('*') if p.is_file()]
    checks.add('diagnostico-NNN folder copied out', folders,
               ', '.join(f'{d} ({sum(1 for p in d.rglob("*") if p.is_file())} files)' for d in folders) or 'none')
    if files:
        size = sum(p.stat().st_size for p in files)
        print(f'  [info] {len(files)} files, {size} bytes')


def check_bench(args, out, serial, checks, wall):
    checks.add('program exit code 0', marker(serial, 'GA36_EXIT') == '0', f'exit {marker(serial, "GA36_EXIT")}')
    table = out / 'frames.csv' if out else None
    if not checks.add('frames.csv written (SS_FRAMELOG)', table and table.exists() and table.stat().st_size,
                      str(table) if table else ''):
        return
    shutil.copyfile(table, args.out / 'frames.csv')
    rows = list(csv.DictReader(io.StringIO(table.read_text(errors='replace'))))
    if not checks.add('frames.csv has rows', rows, f'{len(rows)} rows'):
        return
    rows = rows[args.skip:] if len(rows) > args.skip else rows
    summary = {}
    for column in rows[0]:
        if column is None or column.lower() in ('frame', 'frames', 'index'):
            continue
        try:
            values = sorted(float(r[column]) for r in rows if r.get(column) not in (None, ''))
        except ValueError:
            continue
        if values:
            summary[column] = {'mean': statistics.fmean(values), 'median': statistics.median(values),
                               'p95': values[min(len(values) - 1, int(len(values) * .95))], 'max': values[-1]}
    (args.out / 'bench.json').write_text(json.dumps({'frames_measured': len(rows), 'skipped': args.skip,
                                                     'wall_seconds': round(wall, 1), 'stages': summary}, indent=2))
    print(f'\n  GUEST INSTRUCTIONS, not time (-icount shift=0: 1 ns of guest clock = 1 instruction, so a column in ms '
          f'reads as millions of instructions, in us as thousands). {len(rows)} frames after skipping {args.skip}; '
          f'host wall {wall:.0f} s.')
    print(f'  {"stage":28} {"mean":>12} {"median":>12} {"p95":>12} {"max":>12}')
    for column, s in summary.items():
        print(f'  {column:28} {s["mean"]:12.4f} {s["median"]:12.4f} {s["p95"]:12.4f} {s["max"]:12.4f}')


def check_selftest(args, pkg, out, serial, checks):
    expect = re.compile(args.expect, re.I)
    for binary in ('ss2_ga36', 'ss2_ga36_headless'):
        if not pkg.has(f'{pkg.game_dir}/{binary}'):
            continue
        libs = read_text(out / f'{binary}.libs' if out else None)
        missing = [l.strip() for l in libs.splitlines() if 'not found' in l]
        outside = [l.strip() for l in libs.splitlines()
                   if '=>' in l and 'libarmhf' not in l.split('=>', 1)[1] and 'linux-vdso' not in l]
        checks.add(f'{binary}: loader lists its libraries', marker(serial, f'GA36_LIST {binary}') == '0' and libs,
                   f'exit {marker(serial, f"GA36_LIST {binary}")}, {len(libs.splitlines())} lines')
        checks.add(f'{binary}: every library resolves', not missing, '; '.join(missing) or 'none missing')
        checks.add(f'{binary}: libraries come from libarmhf/', not outside, '; '.join(outside) or 'all bundled')
        sdl = [l.strip() for l in libs.splitlines() if 'libSDL3' in l]
        print(f'  [info] {binary} SDL: {sdl[0] if sdl else "not linked"}')
        code = marker(serial, f'GA36_NOROM {binary}')
        text = read_text(out / f'{binary}.log' if out else None)
        checks.add(f'{binary}: exits on its own without the ROM', code not in (None, '0', '126', '127')
                   and int(code) < 128, f'exit {code}' + (' (crash or signal)' if code and int(code) >= 128 else ''))
        line = expect.search(text)
        checks.add(f'{binary}: prints the missing-ROM message', line,
                   (text[max(0, line.start() - 40):line.end() + 40].strip() if line else text.strip()[-300:]))
    print(f'  [info] launcher without ROM: exit {marker(serial, "GA36_LAUNCHER")}; '
          f'{read_text(out / "launcher.log" if out else None).strip()[-200:]!r}')


def one_line(text):
    return text.strip().replace('\n', ' | ')


def check_ra(args, out, serial, checks):
    import ra_fixture
    test = str(ra_fixture.TEST_ID)
    phase = {name: (out / name if out else Path('missing')) for name, _, _ in RA_PHASES}
    for name, _, _ in RA_PHASES:
        code = marker(serial, f'GA36_RA_{name.upper()}')
        checks.add(f'{name}: launcher exit 0', code == '0', f'exit {code}')
    log = read_text(phase['shipped'] / 'last-run.log')
    found = re.search(r'achievements: (\d+) of the set evaluated here', log)
    checks.add("shipped: the package's set is evaluated", found and found[1] == '52', found[0] if found else 'no line')
    state = read_text(phase['shipped'] / 'retroachievements_estado.txt')
    checks.add("shipped: ReKHCoM's sign-in is used; the real HTTPS path finds no network",
               ra_fixture.TEST_USER in state and 'ReKHCoM' in state and ('Not connected' in state or 'Sin conexion' in state)
               and 'OpenSSL' not in state, one_line(state))
    shot = phase['shipped'] / 'panel.raw'
    if checks.add('shipped: panel with the notice fetched', shot.exists() and shot.stat().st_size == panel_bytes(args)):
        info = fb_to_png(shot, args.out / 'notice.png')
        print(f"  [info] notice picture: {info['non_black']} pixels lit; {args.out / 'notice.png'}")
    offline = phase['offline']
    log = read_text(offline / 'last-run.log')
    checks.add('offline: the test achievement unlocks (local record)', f'local record, "Into the game (test)" ({test})' in log)
    checks.add('offline: queued for the account', f'{test} queued for the account' in log)
    pending = read_text(offline / 'ratest/retroachievements_pending.txt')
    checks.add('offline: retroachievements_pending.txt holds it', pending.splitlines()[:1] == [ra_fixture.TEST_USER]
               and any(line.split()[:1] == [test] for line in pending.splitlines()[1:]), one_line(pending))
    requests = read_text(offline / 'requests.log')
    checks.add('offline: no request got through', requests and all(line.startswith('offline ') for line in requests.splitlines()),
               one_line(requests))
    state = read_text(offline / 'ratest/retroachievements_estado.txt')
    checks.add('offline: estado says not connected, 1 waiting', ('Not connected' in state or 'Sin conexion' in state)
               and re.search(r'(Waiting to be sent|Pendientes de enviar): 1\b', state), one_line(state))
    checks.add('offline: the shared sign-in is not copied', out and not (offline / 'ratest/retroachievements.auth').exists())
    online = phase['online']
    after = [line for line in read_text(online / 'requests.log').splitlines() if not line.startswith('offline ')]
    checks.add('online: signed in, set loaded, queued unlock sent',
               [line.split()[0] for line in after[:3]] == ['login2', 'achievementsets', 'startsession']
               and f'awardachievement {test}' in after, ' | '.join(after))
    state = read_text(online / 'ratest/retroachievements_estado.txt')
    checks.add('online: estado says connected', 'Connected' in state or 'Conectado' in state, one_line(state))
    print(f"  [info] online: pending after the run: {one_line(read_text(online / 'ratest/retroachievements_pending.txt'))!r}")
    rebel = phase['rebel']
    log = read_text(rebel / 'last-run.log')
    checks.add('rebel: Rebel record', f'Rebel record, "Into the game (test)" ({test})' in log)
    checks.add('rebel: no request at all', out and not (rebel / 'requests.log').exists())
    state = read_text(rebel / 'ratest/retroachievements_estado.txt')
    checks.add('rebel: estado says Rebel', 'Rebel' in state, one_line(state))


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='mode', required=True)
    modes = {}
    for mode in DEFAULT_TIMEOUT:
        p = modes[mode] = sub.add_parser(mode)
        p.add_argument('zip', type=Path, help='the GA36 package ZIP')
        if mode != 'no-rom-selftest':
            p.add_argument('--rom', type=Path, required=True, help='your Shining Soul II (Europe) dump')
        p.add_argument('--out', type=Path, required=True, help='new or empty folder for logs and results')
        p.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT[mode], help='seconds for the whole VM')
        p.add_argument('--env', action='append', default=[], metavar='KEY=VALUE', help='extra program variable')
        p.add_argument('--rom-name', default='rom.gba', help='file name under <package>/roms/')
        p.add_argument('--smp', type=int, default=4 if mode == 'diag' else 1)
        p.add_argument('--mem', type=int, default=1024 if mode == 'diag' else 512)
        p.add_argument('--qemu', type=Path, default=QEMU)
        p.add_argument('--kernel', type=Path, default=NETBOOT / 'vmlinuz')
        p.add_argument('--initrd', type=Path, default=NETBOOT / 'initrd.gz')
        p.add_argument('--keep-initrd', action='store_true')
    modes['smoke'].add_argument('--frames', type=int, default=600, help='SS_QUIT frame (default 600)')
    modes['smoke'].add_argument('--direct', action='store_true',
                                help='start ss2_ga36 through the loader instead of the launcher script')
    modes['smoke'].add_argument('--input', type=Path, help='SS_INPUT_FILE script')
    modes['smoke'].add_argument('--kms', action='store_true',
                                help='give the VM a virtio-gpu display and the program /dev/fb0 (tests the KMS path)')
    modes['hash'].add_argument('--frames', type=int, required=True)
    modes['hash'].add_argument('--input', type=Path, help='SS_INPUT_FILE script')
    modes['hash'].add_argument('--compare', type=Path, help='reference text with "frame N hash H" lines')
    modes['bench'].add_argument('--frames', type=int, required=True, help='SS_QUIT frame')
    modes['bench'].add_argument('--skip', type=int, default=60, help='first frames left out of the statistics')
    modes['bench'].add_argument('--direct', action='store_true',
                                help='start ss2_ga36 through the loader instead of the launcher script')
    modes['ra'].add_argument('--frames', type=int, default=3600, help='SS_QUIT of the offline, online and Rebel runs')
    modes['ra'].add_argument('--input', type=Path, required=True, help='SS_INPUT_FILE script (tools/tests/play_input.txt)')
    modes['no-rom-selftest'].add_argument('--expect', default=r'cannot open the ROM',
                                          help='regex the program must print without a ROM')
    args = parser.parse_args()
    if args.mode == 'bench' and args.smp != 1:
        parser.error('bench counts instructions on one vCPU; leave --smp at 1')
    for path, what in ((args.zip, 'package'), (args.qemu, 'QEMU'), (args.kernel, 'kernel'), (args.initrd, 'initrd'),
                       (getattr(args, 'rom', None), 'ROM'), (getattr(args, 'input', None), 'input script'),
                       (getattr(args, 'compare', None), 'reference')):
        if path is not None and not path.is_file():
            sys.exit(f'ga36_vm_test: {what} not found: {path}')
    if args.out.exists() and any(args.out.iterdir()):
        sys.exit(f'ga36_vm_test: {args.out} is not empty; give a new folder')
    args.out.mkdir(parents=True, exist_ok=True)

    pkg = Package(args.zip)
    checks = Checks()
    print(f'{args.mode}: {args.zip} (game folder {pkg.prefix}{pkg.game_dir}/)')
    info = elf_summary(pkg.read(f'{pkg.game_dir}/ss2_ga36'))
    checks.add('ss2_ga36 is a 32-bit ARM hard-float ELF', info.get('arm') and info.get('hard_float'),
               json.dumps(info))
    checks.add(f'{pkg.game_dir}/{LOADER} is in the package', pkg.has(f'{pkg.game_dir}/{LOADER}'))
    if args.mode in ('smoke', 'bench', 'no-rom-selftest', 'ra') and not getattr(args, 'direct', False):
        checks.add(f'"{LAUNCHER}" is in the package', pkg.has(LAUNCHER))
    if args.mode == 'diag':
        checks.add(f'"{DIAGNOSTIC}" is in the package', pkg.has(DIAGNOSTIC))
    if not checks.ok:
        sys.exit('ga36_vm_test: the package is incomplete; not booting the VM')

    initrd = args.out / 'initrd.gz'
    build_initrd(args, pkg, initrd)
    print(f'  booting QEMU (timeout {args.timeout} s, {args.smp} vCPU, {args.mem} MiB'
          f'{", -icount shift=0" if args.mode == "bench" else ""}) ...', flush=True)
    code, timed_out, wall = run_vm(args, initrd)
    if not args.keep_initrd:
        initrd.unlink(missing_ok=True)   # it holds the ROM
    serial = read_text(args.out / 'serial.log').replace('\r', '')
    print(f'  QEMU exit {code} after {wall:.0f} s; serial log {args.out / "serial.log"}')
    checks.add('VM finished before the timeout', not timed_out, f'{args.timeout} s' if timed_out else f'{wall:.0f} s')
    booted = re.search(r'GA36_VM_BOOTED (.*)', serial)
    checks.add('guest booted and ran the init script', booted,
               booted.group(1).strip() if booted else 'no GA36_VM_BOOTED line: see serial.log')
    checks.add('guest reached the end of the script', 'GA36_DONE' in serial)
    out = fetch(args, serial)
    checks.add('results copied out of the VM', out and out.exists(),
               f'via {marker(serial, "GA36_OUTBOX") or "?"} into {args.out / "fetched"}')
    if out and (out / 'program.log').exists():
        shutil.copyfile(out / 'program.log', args.out / 'program.log')

    if args.mode == 'smoke':
        check_smoke(args, out, serial, checks)
    elif args.mode == 'hash':
        check_hash(args, out, serial, checks)
    elif args.mode == 'diag':
        check_diag(args, out, serial, checks)
    elif args.mode == 'bench':
        check_bench(args, out, serial, checks, wall)
    elif args.mode == 'ra':
        check_ra(args, out, serial, checks)
    else:
        check_selftest(args, pkg, out, serial, checks)

    (args.out / 'result.json').write_text(json.dumps({'mode': args.mode, 'zip': str(args.zip), 'ok': checks.ok,
                                                      'qemu_exit': code, 'wall_seconds': round(wall, 1),
                                                      'checks': checks.items}, indent=2))
    if not checks.ok:
        failed = [item['check'] for item in checks.items if not item['ok']]
        sys.exit(f'ga36_vm_test {args.mode}: FAILED ({"; ".join(failed)}); see {args.out}')
    print(f'ga36_vm_test {args.mode}: PASSED; results in {args.out}')


if __name__ == '__main__':
    main()
