#!/usr/bin/env python3
"""Link the dev32 executable of a game (milestone 1). Run `build.py compile` first.

  link.py [--game ss2]

What the game's objects still need is sorted into four groups:

  memory    gUnk_02/03/04... symbols: fixed GBA addresses. They become absolute symbols; the runtime maps
            real memory there before the game starts.
  rom       gUnk_08... symbols and named ROM labels: the user's ROM is mapped at 0x08000000, so these are
            absolute symbols too.
  runtime   functions the runtime implements (runtime/src/*.c).
  stubs     functions nobody implements yet. A generated stub reports its name the first time it is called.
            This list is the work list of the next milestones; it is written to build/.../stubs.txt.

A symbol that fits no group stops the link with its name: nothing is guessed.
"""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build

ROOT = build.ROOT
SDL_HINTS = [os.environ.get("SHININGSOURCE_SDL3", ""), "D:/01Claude/toolchain/SDL3-3.4.18"]
ADDRESS = re.compile(r'gUnk_(0[2-9A-E][0-9A-F]{6})([-+]\d+)?$')


def nm(args):
    exe = Path(build.clang()).with_name("llvm-nm.exe")
    return subprocess.run([str(exe), *args], capture_output=True, text=True).stdout


def named_addresses(game):
    """Named symbols with a fixed address: the decomp's RAM symbol script, and ROM labels placed by `.incbin`."""
    decomp, found = build.GAMES[game], {}
    script = decomp / "sym_ram.ld"
    if script.exists():
        for name, value in re.findall(r'^(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;', script.read_text(encoding="utf-8"), re.M):
            found[name] = int(value, 16)
    for path in sorted((decomp / "data").glob("*.s")):
        text = path.read_text(encoding="utf-8")
        for name, offset in re.findall(r'^([A-Za-z_]\w*):\s*\n\s*\.incbin\s+"baserom\.gba"\s*,\s*(0x[0-9A-Fa-f]+)', text, re.M):
            found.setdefault(name, 0x08000000 + int(offset, 16))
    extra = ROOT / "games" / game / "port/symbols.json"
    if extra.exists():
        found.update({k: int(v, 16) for k, v in json.loads(extra.read_text(encoding="utf-8")).items()})
    return found


def function_names(game):
    cfg = build.GAMES[game] / "tools/config/functions.cfg"
    names = set()
    for line in cfg.read_text(encoding="utf-8").splitlines():
        fields = line.split("#")[0].split()
        if len(fields) >= 2:
            names.add(fields[2] if len(fields) > 2 else "sub_%08X" % (int(fields[1], 16) & ~1))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    args = parser.parse_args()
    out = ROOT / "build/dev32" / args.game
    objects = sorted((out / "obj").glob("*.o"))
    if not objects:
        sys.exit("no objects: run tools/build.py compile")
    cc = build.clang()

    sdl = next((Path(h) / "i686-w64-mingw32" for h in SDL_HINTS if h and (Path(h) / "i686-w64-mingw32/lib").is_dir()), None)
    if sdl is None:
        sys.exit("SDL3 development files not found: set SHININGSOURCE_SDL3 to the SDL3 mingw package folder")
    # the runtime, compiled with the host's own headers
    runtime_objects = []
    (out / "rt").mkdir(exist_ok=True)
    for source in sorted((ROOT / "runtime/src").glob("*.c")):
        target = out / "rt" / (source.stem + ".o")
        run = subprocess.run([cc, "--target=i686-w64-mingw32", "-O2", "-g", "-Wall", "-I", str(ROOT / "runtime/include"),
                              "-I", str(sdl / "include"),
                              "-c", str(source), "-o", str(target)], capture_output=True, text=True)
        if run.returncode:
            sys.exit(run.stderr)
        runtime_objects.append(target)

    strip = lambda name: name[1:] if name.startswith("_") else name
    defined = {strip(l.split()[-1]) for l in nm(["--defined-only", *map(str, objects + runtime_objects)]).splitlines() if len(l.split()) == 3}
    undefined = lambda files: {strip(l.split()[-1]) for l in nm(["--undefined-only", *map(str, files)]).splitlines() if len(l.split()) == 2}
    named, functions = named_addresses(args.game), function_names(args.game)
    # the runtime also reads game memory by name; its other undefined symbols are the host's C library
    wanted = undefined(objects) | {n for n in undefined(runtime_objects) if ADDRESS.match(n) or n in named or n in functions}
    wanted -= defined
    host = {"memcpy", "memset", "memmove", "abs", "__divsi3", "__modsi3", "__udivsi3", "__umodsi3", "__divdi3", "__moddi3",
            "__udivdi3", "__umoddi3", "__muldi3", "__ashldi3", "__ashrdi3", "__lshrdi3"}
    absolute, stubs, unknown = {}, [], []
    for name in sorted(wanted):
        m = ADDRESS.match(name)
        if m:
            absolute[name] = int(m[1], 16) + int(m[2] or 0)
        elif name in named:
            absolute[name] = named[name]
        elif name in host:
            continue
        elif name in functions or re.fullmatch(r"sub_08[0-9A-F]{6}", name):
            stubs.append(name)
        else:
            unknown.append(name)
    if unknown:
        sys.exit("symbols that fit no group (add them to games/%s/port/symbols.json or implement them):\n  %s"
                 % (args.game, "\n  ".join(unknown)))

    lines = ['\t.globl "_%s"\n"_%s" = 0x%08X' % (n, n, a) for n, a in sorted(absolute.items())]
    (out / "absolute.s").write_text("\n".join(lines) + "\n", encoding="utf-8")
    body = ['#include "shiningsource/runtime.h"', ""]
    body += ["int %s(void) { static int seen; ss_stub(\"%s\", &seen); return 0; }" % (n, n) for n in stubs]
    (out / "stubs.c").write_text("\n".join(body) + "\n", encoding="utf-8")
    (out / "stubs.txt").write_text("\n".join(stubs) + "\n", encoding="utf-8")
    # table of game functions for pointers stored in the ROM data (runtime/src/codeptr.c)
    entries = []
    for line in (build.GAMES[args.game] / "tools/config/functions.cfg").read_text(encoding="utf-8").splitlines():
        fields = line.split("#")[0].split()
        if len(fields) < 2:
            continue
        address = int(fields[1], 16) & ~1
        name = fields[2] if len(fields) > 2 else "sub_%08X" % address
        if name in defined or name in stubs:
            entries.append((address | (fields[0] == "thumb"), name))
    entries.sort()
    ranges = json.loads((ROOT / "games" / args.game / "port/code_pointer_ranges.json").read_text(encoding="utf-8"))["ranges"]
    table = ['#include <stdint.h>', 'struct ss_code_entry { uint32_t gba; void (*native)(void); };']
    table += ["void %s(void);" % n for n in sorted({n for _, n in entries})]
    table += ["const struct ss_code_entry ss_code_table[] = {"] + ["    { 0x%08Xu, %s }," % e for e in entries] + ["};"]
    table += ["const unsigned ss_code_count = %d;" % len(entries),
              "const uint32_t ss_code_ranges[][2] = { %s };" % ", ".join("{ %s, %s }" % (a, b) for a, b in ranges),
              "const unsigned ss_code_range_count = %d;" % len(ranges)]
    (out / "codetable.c").write_text("\n".join(table) + "\n", encoding="utf-8")
    extra = []
    for source in ("absolute.s", "stubs.c", "codetable.c"):
        target = out / "rt" / (source.split(".")[0] + ".o")
        run = subprocess.run([cc, "--target=i686-w64-mingw32", "-O1", "-I", str(ROOT / "runtime/include"), "-c", str(out / source),
                              "-o", str(target)], capture_output=True, text=True)
        if run.returncode:
            sys.exit(run.stderr[:4000])
        extra.append(target)
    exe = out / ("%s_dev32.exe" % args.game)
    response = out / "link.rsp"
    response.write_text("\n".join('"%s"' % str(p).replace("\\", "/") for p in objects + runtime_objects + extra), encoding="utf-8")
    run = subprocess.run([cc, "--target=i686-w64-mingw32", "-g", "@" + str(response), "-o", str(exe),
                          "-Wl,--image-base=0x01800000", "-Wl,--disable-dynamicbase", "-Wl,--stack,0x400000",
                          # SDL is loaded on first use, after the runtime has reserved the GBA address ranges
                          "-L", str(sdl / "lib"), "-lSDL3", "-Wl,--delayload=SDL3.dll"],
                         capture_output=True, text=True)
    if run.returncode:
        errors = run.stderr.splitlines()
        print("\n".join(errors[:40]))
        sys.exit("link failed (%d lines of diagnostics)" % len(errors))
    shutil.copy2(sdl / "bin/SDL3.dll", out / "SDL3.dll")
    print("%s: %d absolute symbols (memory and ROM), %d functions still stubbed" % (exe.relative_to(ROOT), len(absolute), len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
