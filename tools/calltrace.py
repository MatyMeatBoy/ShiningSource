#!/usr/bin/env python3
"""calltrace.py TRACE_A BINARY_A TRACE_B BINARY_B : compare two call traces (see runtime/core/trace.c).

Each trace is the list of game functions entered during one frame, as addresses of its own binary. Addresses are
turned into names with llvm-nm, and the first position where the two name sequences differ is printed with
some context: the function entered just before is where the builds took different decisions.
"""
import bisect, struct, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build


def names(trace, binary):
    nm = Path(build.clang()).with_name("llvm-nm.exe")
    table = []
    for line in subprocess.run([str(nm), "-n", binary], capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in "Tt":
            table.append((int(fields[0], 16), fields[2].lstrip("_")))
    table.sort()
    starts = [a for a, _ in table]
    raw = Path(trace).read_bytes()
    out = []
    for (address,) in struct.iter_unpack("<I", raw):
        i = bisect.bisect_right(starts, address) - 1
        out.append(table[i][1] if i >= 0 else "%08X" % address)
    return out


def main():
    a, b = names(sys.argv[1], sys.argv[2]), names(sys.argv[3], sys.argv[4])
    print("%d calls in A, %d in B" % (len(a), len(b)))
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print("first difference at call %d" % i)
            for j in range(max(0, i - 12), i):
                print("    same   %s" % a[j])
            for j in range(i, min(len(a), len(b), i + 8)):
                print("    A %-28s B %s" % (a[j], b[j]))
            return 1
    print("identical up to the shorter trace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
