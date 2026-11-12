#!/usr/bin/env python3
"""crash_names.py [LOG] [IMAGE]: name the game functions in a crash or hang report.

LOG defaults to build/win64/ss2/shiningsource_crash.log and IMAGE to the game image next to it. Every address of
the report that lies inside the image is printed with the function that contains it. The image must be the one
that was running: a report names the build time of its host for that reason.
"""
import bisect, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build


def main():
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else build.ROOT / "build/win64/ss2/shiningsource_crash.log"
    image = Path(sys.argv[2]) if len(sys.argv) > 2 else log.with_name("ss2_game.elf")
    nm = Path(build.clang()).with_name("llvm-nm.exe")
    table = []
    for line in subprocess.run([str(nm), "-n", str(image)], capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in "Tt":
            table.append((int(fields[0], 16), fields[2].lstrip("_")))
    if not table:
        sys.exit("no symbols in " + str(image))
    table.sort()
    starts = [a for a, _ in table]
    low, high = starts[0], starts[-1] + 0x10000
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        print(line)
        for word in re.findall(r'\b(?:0{8})?([0-9A-F]{8})\b', line):
            address = int(word, 16)
            if low <= address < high:
                start, name = table[bisect.bisect_right(starts, address) - 1]
                print("    %08X  %s+0x%X" % (address, name, address - start))
    return 0


if __name__ == "__main__":
    sys.exit(main())
