#!/usr/bin/env python3
"""Run commands in the consoles build machine (QEMU) and bring files back.

  console_vm.py --share DIR --run "bash /root/share/build.sh" --fetch /root/share/out.vpk --out DIR

The machine is ReKHCoM's consoles image (Debian 12 x86-64 with devkitA64 + libnx in /opt/devkitpro and VitaSDK
in /root/consoles/vitasdk): SHININGSOURCE_CONSOLE_VM, default D:/01Claude/ReKHCoM-build/ps5/ps5env.qcow2. It is
opened with snapshot=on: everything this run writes goes to a temporary file and is thrown away, so the image is
never changed and nothing of a build stays in it. No network is given to it.

The method is ReKHCoM's (port/tools/linux_vm_test.py): the shared folder travels as a tar archive used as a raw
disk, the commands are typed on the serial console, and the fetched files come back as a tar archive written
onto a second raw disk.
"""
import argparse, os, queue, re, subprocess, sys, tarfile, threading, time
from pathlib import Path

QEMU = Path(os.environ.get("SHININGSOURCE_QEMU", "D:/01Claude/ReKHCoM-build/toolchain/qemu/runtime"))
IMAGE = Path(os.environ.get("SHININGSOURCE_CONSOLE_VM", "D:/01Claude/ReKHCoM-build/ps5/ps5env.qcow2"))
ARM64_IMAGE = Path(os.environ.get("SHININGSOURCE_ARM64_VM", "D:/01Claude/ReKHCoM-build/linux/vm/debian-12-nocloud-arm64.qcow2"))


class Console:
    def __init__(self, proc):
        self.proc, self.q, self.buf = proc, queue.Queue(), ""
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            b = self.proc.stdout.read(1)
            if not b:
                self.q.put(None)
                return
            self.q.put(b.decode("latin-1"))

    def expect(self, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            base = max(0, len(self.buf) - 4096)
            m = re.search(pattern, self.buf[base:])
            if m:
                out, self.buf = self.buf[:base + m.end()], self.buf[base + m.end():]
                return out
            try:
                c = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            if c is None:
                raise SystemExit("QEMU exited:\n" + self.buf[-2000:])
            self.buf += c
        raise SystemExit("timeout waiting for %r:\n%s" % (pattern, self.buf[-3000:]))

    def send(self, text):
        data = text.encode()            # the emulated serial port drops input that arrives in one burst
        for i in range(0, len(data), 8):
            self.proc.stdin.write(data[i:i + 8])
            self.proc.stdin.flush()
            time.sleep(0.03)

    def run(self, cmd, timeout=600):
        marker = "__DONE_%d__" % int(time.time() * 1000)
        self.send("%s; echo %s$?\n" % (cmd, marker))
        out = self.expect(marker + r"(\d+)", timeout)
        return out, int(re.search(marker + r"(\d+)", out).group(1))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--share", type=Path, required=True)
    ap.add_argument("--run", action="append", default=[])
    ap.add_argument("--fetch", action="append", default=[])
    ap.add_argument("--out", type=Path, default=Path("."))
    ap.add_argument("--machine", choices=["x86_64", "aarch64"], default="x86_64")
    ap.add_argument("--accel", default="whpx,kernel-irqchip=off")
    ap.add_argument("--run-timeout", type=int, default=1800)
    ap.add_argument("--mem", default="3072")
    ap.add_argument("--cpus", default="2")
    args = ap.parse_args()
    image = IMAGE if args.machine == "x86_64" else ARM64_IMAGE
    if not image.exists() or not (QEMU / "qemu-system-x86_64.exe").exists():
        sys.exit("missing: %s or QEMU in %s" % (image, QEMU))
    archive = args.share.with_suffix(".tar")
    with tarfile.open(archive, "w", format=tarfile.GNU_FORMAT) as t:
        for f in sorted(args.share.rglob("*")):
            info = t.gettarinfo(f, f.relative_to(args.share).as_posix())
            info.mode = 0o755 if f.is_dir() or f.suffix in ("", ".sh") else 0o644
            if f.is_file():
                with open(f, "rb") as fh:
                    t.addfile(info, fh)
            else:
                t.addfile(info)
    with open(archive, "ab") as fh:
        fh.write(b"\0" * (-fh.tell() % 1048576))
    outbox = archive.with_name(archive.stem + "-outbox.img")
    with open(outbox, "wb") as fh:
        fh.truncate(1 << 28)
    drives = ["-drive", "file=%s,if=virtio,snapshot=on" % image.as_posix(),
              "-drive", "file=%s,format=raw,if=virtio,readonly=on" % archive.as_posix(),
              "-drive", "file=%s,format=raw,if=virtio" % outbox.as_posix(), "-nographic", "-nic", "none"]
    if args.machine == "x86_64":
        cmd = [str(QEMU / "qemu-system-x86_64.exe"), "-m", args.mem, "-smp", args.cpus, "-accel", args.accel, "-accel", "tcg", *drives]
    else:
        cmd = [str(QEMU / "qemu-system-aarch64.exe"), "-M", "virt", "-cpu", "max", "-accel", "tcg,thread=multi", "-m", args.mem,
               "-smp", args.cpus, "-bios", str(QEMU / "share/edk2-aarch64-code.fd"), *drives]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print("qemu pid %d" % proc.pid, flush=True)
    con, failed = Console(proc), 0
    try:
        con.expect(r"login: ", 900)
        con.send("root\n")
        con.expect(r"# ", 120)
        con.send('stty -echo cols 250; export PS1="# "\n')
        con.expect(r"# ", 30)
        out, code = con.run("rm -rf /root/share && mkdir -p /root/share && tar -xf /dev/vdb -C /root/share", 300)
        for c in args.run:
            out, code = con.run(c, args.run_timeout)
            print("$ %s\n%s\n[exit %d]" % (c, out.strip(), code), flush=True)
            failed |= code
        args.out.mkdir(parents=True, exist_ok=True)
        if args.fetch:
            con.run("tar -cf /dev/vdc -C / %s; sync" % " ".join(f.lstrip("/") for f in args.fetch), 600)
        con.send("poweroff\n")
        try:
            proc.wait(120)
        except subprocess.TimeoutExpired:
            pass
        if args.fetch:
            with tarfile.open(outbox) as t:
                for f in args.fetch:
                    try:
                        data = t.extractfile(f.lstrip("/")).read()
                    except (KeyError, AttributeError):
                        print("could not fetch", f)
                        failed = 1
                        continue
                    (args.out / Path(f).name).write_bytes(data)
                    print("fetched", f, len(data), "bytes")
    finally:
        if proc.poll() is None:
            proc.kill()                 # this run's own machine, by its handle
        outbox.unlink(missing_ok=True)
        archive.unlink(missing_ok=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
