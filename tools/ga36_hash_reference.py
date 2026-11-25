#!/usr/bin/env python3
"""Make (or check) the hash reference the GA36 diagnostic's hash cases are compared with.

  ga36_hash_reference.py [--exe build/win64/ss2/ss2_win64.exe] [--rom ROM] [--out tools/tests/ga36_hash_reference.txt]
                         [--frames 3000] [--check]

Runs the Windows 64-bit build without a window over tools/tests/play_input.txt with exactly the variables of the
diagnostic's hash cases (platform/ga36/diagnostic/"ShiningSource GA36 - Diagnostico.sh", run(): SS_SKIP=0, SS_NET=
(empty), SS_FRAMELOG=<work>/frames.csv, then SS_FRAMES=3000 SS_HASH_ALL=1 SS_INPUT_FILE=play_input.txt) in a fresh
scratch folder, and keeps only the "frame N  hash XXXXXXXX" lines. --check compares a new run with the stored file
instead of writing it (the determinism of the Windows build itself). The ROM is only read.
"""
import argparse, os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = "D:/01Claude/GBA/Shining Soul II (Europe) (En,Fr,De,Es,It).gba"
HASH_RE = re.compile(r"^frame\s+\d+\s+hash\s+[0-9A-F]{8}\s+stubs reached\s+\d+$")


def run(exe, rom, frames, play):
    with tempfile.TemporaryDirectory(prefix="ss_hashref_") as work:
        env = {k: v for k, v in os.environ.items() if not k.startswith("SS_")}
        env.update({"SS_SKIP": "0", "SS_NET": "", "SS_FRAMELOG": str(Path(work) / "frames.csv"),
                    "SS_FRAMES": str(frames), "SS_HASH_ALL": "1", "SS_INPUT_FILE": str(play)})
        done = subprocess.run([str(exe), str(rom)], cwd=work, env=env, capture_output=True, text=True, timeout=600)
        lines = [l.rstrip() for l in done.stderr.splitlines() if HASH_RE.match(l.strip())]
        if done.returncode or len(lines) < frames:
            sys.stderr.write(done.stderr[-2000:])
            sys.exit("run failed: exit %d, %d hash lines" % (done.returncode, len(lines)))
        return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", default=str(ROOT / "build/win64/ss2/ss2_win64.exe"))
    parser.add_argument("--rom", default=os.environ.get("SS_ROM", DEFAULT_ROM))
    parser.add_argument("--out", default=str(ROOT / "tools/tests/ga36_hash_reference.txt"))
    parser.add_argument("--frames", type=int, default=3000)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    lines = run(Path(args.exe).resolve(), Path(args.rom), args.frames, ROOT / "tools/tests/play_input.txt")
    text = "\n".join(lines) + "\n"
    if args.check:
        old = Path(args.out).read_text().splitlines()
        first = next((i for i, (a, b) in enumerate(zip(lines, old)) if a != b), None)
        print("same as %s (%d lines)" % (args.out, len(old)) if first is None and len(old) == len(lines)
              else "DIFFERENT from line %s" % (first + 1 if first is not None else min(len(old), len(lines)) + 1))
        return 0 if first is None and len(old) == len(lines) else 1
    Path(args.out).write_text(text, newline="\n")
    print("%s: %d hash lines" % (args.out, len(lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
