#!/usr/bin/env python3
"""crash_names.py [LOG] [IMAGE] [HOST]: name the functions in a crash or hang report.

LOG defaults to build/win64/ss2/shiningsource_crash.log, IMAGE to the game image next to it and HOST to the host
program next to it (ss2_win64.exe). Every address of the report that lies inside the image or inside the host's
code is printed with the function that contains it (a crash on a thread of the host, such as a worker of the
achievements, is at a host address). Image and host must be the ones that were running: a report names the build
time of its host for that reason.

crash_names.py --ga36 [LOG] [BINARY]: the armhf Linux host's report (runtime/android32/host.c, "crash:" lines, in
user/shiningsource_crash.log and last-run.log). LOG defaults to user/shiningsource_crash.log, BINARY to
build/ga36/ss2/ss2_ga36 (it must be the build that crashed). Every "ADDRESS (FILE+0xOFFSET" whose FILE is the
binary, or a library of BINARY's libarmhf/, is named with llvm-symbolizer (function, and file:line where the
build has -g: the host and platform code do). Return addresses on the stack are looked up one byte back, at the
call itself.
"""
import bisect, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build


def ga36(arguments):
    log = Path(arguments[0]) if arguments else Path("user/shiningsource_crash.log")
    binary = Path(arguments[1]) if len(arguments) > 1 else build.ROOT / "build/ga36/ss2/ss2_ga36"
    symbolizer = Path(build.clang()).with_name("llvm-symbolizer.exe")
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    wanted = {}                                        # (object, offset) -> name
    found = []
    pattern = re.compile(r'0x([0-9a-f]{8}) \(([^()]*?)\+0x([0-9a-f]{8})(, code)?\)')
    for index, line in enumerate(lines):
        for match in pattern.finditer(line):
            name, offset = Path(match[2]).name, int(match[3], 16)
            target = binary if name == binary.name else binary.parent / "libarmhf" / name
            if not match[4] or not target.is_file():
                continue
            # a return address (lr, a stack word) points after the call: one byte back is inside it
            back = 1 if ("crash: stack" in line or line.startswith("crash: lr")) else 0
            key = (str(target), (offset & ~1) - back if back else offset & ~1)
            wanted[key] = None
            found.append((index, match[1], key))
    for target in sorted({key[0] for key in wanted}):
        keys = [key for key in wanted if key[0] == target]
        result = subprocess.run([str(symbolizer), "--obj=" + target, "--functions=linkage", "--demangle", "--relativenames"],
                                input="".join("0x%x\n" % key[1] for key in keys), capture_output=True, text=True)
        blocks = result.stdout.strip("\n").split("\n\n")
        for key, block in zip(keys, blocks):
            parts = block.splitlines()
            wanted[key] = "%s  %s" % (parts[0] if parts else "?", parts[1] if len(parts) > 1 else "")
    notes = {}
    for index, address, key in found:
        notes.setdefault(index, []).append("    %s  %s+0x%x  %s" % (address, Path(key[0]).name, key[1], wanted[key]))
    for index, line in enumerate(lines):
        print(line)
        for note in notes.get(index, []):
            print(note)
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--ga36":
        return ga36(sys.argv[2:])
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
