#!/usr/bin/env python3
"""crash_names.py [LOG] [IMAGE] [HOST]: name the functions in a crash or hang report.

LOG defaults to build/win64/ss2/shiningsource_crash.log, IMAGE to the game image next to it and HOST to the host
program next to it (ss2_win64.exe). Every address of the report that lies inside the image or inside the host's
code is printed with the function that contains it (a crash on a thread of the host, such as a worker of the
achievements, is at a host address). Image and host must be the ones that were running: a report names the build
time of its host for that reason.
"""
import bisect, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build


def main():
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else build.ROOT / "build/win64/ss2/shiningsource_crash.log"
    image = Path(sys.argv[2]) if len(sys.argv) > 2 else log.with_name("ss2_game.elf")
    host = Path(sys.argv[3]) if len(sys.argv) > 3 else log.with_name("ss2_win64.exe")
    nm = Path(build.clang()).with_name("llvm-nm.exe")

    def symbols(binary):
        table = []
        for line in subprocess.run([str(nm), "-n", str(binary)], capture_output=True, text=True).stdout.splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[1] in "Tt":
                table.append((int(fields[0], 16), fields[2].lstrip("_")))
        table.sort()
        return table, [a for a, _ in table]

    table, starts = symbols(image)
    if not table:
        sys.exit("no symbols in " + str(image))
    ranges = [(starts[0], starts[-1] + 0x10000, table, starts, "")]
    if host.is_file():
        host_table, host_starts = symbols(host)
        if host_table:
            ranges.append((host_starts[0], host_starts[-1] + 0x1000, host_table, host_starts, "  (host)"))
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        print(line)
        for word in re.findall(r'\b(?:0{8})?([0-9A-F]{8})\b', line):
            address = int(word, 16)
            for low, high, names, firsts, where in ranges:
                if low <= address < high:
                    start, name = names[bisect.bisect_right(firsts, address) - 1]
                    print("    %08X  %s+0x%X%s" % (address, name, address - start, where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
