#!/usr/bin/env python3
"""Build the 64-bit Windows version (milestone 5): the game image and its host.

  build_win64.py [--game ss2] [-j N]

Outputs in build/win64/<game>/:
  ss2_game.elf   the game: 64-bit code with 32-bit pointers (x32), linked at 0x0A000000
  ss2_win64.exe  the host: maps the GBA memory, loads ROM and image, provides the services
  SDL3.dll

The game sources are the same generated sources as dev32 without the symbol prefix of 32-bit Windows.
"""
import argparse, concurrent.futures, json, os, re, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build, link

ROOT = build.ROOT
GUEST_TARGET = ["--target=x86_64-unknown-linux-gnux32", "-ffreestanding", "-fno-pic", "-mno-red-zone", "-fno-stack-protector",
                "-fno-asynchronous-unwind-tables"]


def run(cmd, what):
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode:
        print(done.stderr[-3000:])
        sys.exit("failed: " + what)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    cc = build.clang()
    bindir = Path(cc).parent
    out = ROOT / "build/win64" / args.game
    (out / "obj").mkdir(parents=True, exist_ok=True)
    (out / "rt").mkdir(exist_ok=True)

    # 1. game modules
    game_flags = [f for f in build.CFLAGS if not f.startswith("--target")] + GUEST_TARGET
    gen = build.prepare(args.game, prefix=False, abi_flags=game_flags)
    replaced = build.port_config(args.game)["runtime"]
    sources = [p for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced]

    def compile_game(path):
        target = out / "obj" / (path.stem + ".o")
        done = subprocess.run([cc, *game_flags, "-iquote", str(gen / "include"), "-I", str(gen / "include"), "-c", str(path),
                               "-o", str(target)], capture_output=True, text=True, cwd=gen)
        return path.stem, done.returncode, done.stderr
    failed = []
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        for name, code, errors in pool.map(compile_game, sources):
            if code:
                failed.append((name, errors))
    if failed:
        for name, errors in failed[:3]:
            print(name, "\n", "\n".join(l for l in errors.splitlines() if "error:" in l)[:1500])
        sys.exit("%d game module(s) do not compile for the 64-bit image" % len(failed))
    objects = sorted((out / "obj").glob("*.o"))

    # 2. runtime core and the image's platform layer
    runtime_objects = []
    rt_flags = [*GUEST_TARGET, "-O2", "-g", "-Wall", "-I", str(ROOT / "runtime/guest/include"), "-I", str(ROOT / "runtime/include"),
                "-I", str(ROOT / "runtime/guest")]
    for source in sorted((ROOT / "runtime/core").glob("*.c")) + sorted((ROOT / "runtime/guest").glob("*.c")) + sorted((ROOT / "runtime/guest").glob("*.S")):
        target = out / "rt" / (source.stem + ".o")
        run([cc, *rt_flags, "-c", str(source), "-o", str(target)], source.name)
        runtime_objects.append(target)

    # 3. what the game still needs: absolute symbols, stubs, the code-pointer table (same rules as dev32)
    nm = lambda flags, files: link.nm([*flags, *map(str, files)])
    defined = {l.split()[-1] for l in nm(["--defined-only"], objects + runtime_objects).splitlines() if len(l.split()) == 3}
    undefined = lambda files: {l.split()[-1] for l in nm(["--undefined-only"], files).splitlines() if len(l.split()) == 2}
    named, functions = link.named_addresses(args.game), link.function_names(args.game)
    wanted = (undefined(objects) | {n for n in undefined(runtime_objects) if link.ADDRESS.match(n) or n in named or n in functions}) - defined
    absolute, stubs, unknown = {}, [], []
    for name in sorted(wanted):
        m = link.ADDRESS.match(name)
        if m:
            absolute[name] = int(m[1], 16) + int(m[2] or 0)
        elif name in named:
            absolute[name] = named[name]
        elif name in functions or re.fullmatch(r"sub_08[0-9A-F]{6}", name):
            stubs.append(name)
        elif name.startswith("__real_") or name.startswith("__wrap_"):
            continue
        else:
            unknown.append(name)
    if unknown:
        sys.exit("symbols that fit no group:\n  " + "\n  ".join(unknown))
    (out / "absolute.s").write_text("\n".join('\t.globl "%s"\n"%s" = 0x%08X' % (n, n, a) for n, a in sorted(absolute.items()))
                                    + '\n\t.section .note.GNU-stack,"",@progbits\n', encoding="utf-8")
    (out / "stubs.c").write_text('#include "shiningsource/runtime.h"\n' + "\n".join(
        "int %s(void) { static int seen; ss_stub(\"%s\", &seen); return 0; }" % (n, n) for n in stubs) + "\n", encoding="utf-8")
    entries = []
    for line in (build.GAMES[args.game] / "tools/config/functions.cfg").read_text(encoding="utf-8").splitlines():
        fields = line.split("#")[0].split()
        if len(fields) >= 2:
            address = int(fields[1], 16) & ~1
            name = fields[2] if len(fields) > 2 else "sub_%08X" % address
            if name in defined or name in stubs:
                entries.append((address | (fields[0] == "thumb"), name))
    entries.sort()
    ranges = json.loads((ROOT / "games" / args.game / "port/code_pointer_ranges.json").read_text(encoding="utf-8"))["ranges"]
    table = ["#include <stdint.h>", "struct ss_code_entry { uint32_t gba; void (*native)(void); };"]
    table += ["void %s(void);" % n for n in sorted({n for _, n in entries})]
    table += ["const struct ss_code_entry ss_code_table[] = {"] + ["    { 0x%08Xu, %s }," % e for e in entries] + ["};"]
    table += ["const unsigned ss_code_count = %d;" % len(entries),
              "const uint32_t ss_code_ranges[][2] = { %s };" % ", ".join("{ %s, %s }" % (a, b) for a, b in ranges),
              "const unsigned ss_code_range_count = %d;" % len(ranges)]
    (out / "codetable.c").write_text("\n".join(table) + "\n", encoding="utf-8")
    extra = []
    for source in ("absolute.s", "stubs.c", "codetable.c"):
        target = out / "rt" / (source.split(".")[0] + ".o")
        run([cc, *rt_flags, "-w", "-c", str(out / source), "-o", str(target)], source)
        extra.append(target)

    # 4. link the image
    image = out / ("%s_game.elf" % args.game)
    response = out / "link.rsp"
    response.write_text("\n".join('"%s"' % str(p).replace("\\", "/") for p in objects + runtime_objects + extra), encoding="utf-8")
    run([str(bindir / "ld.lld.exe"), "-m", "elf32_x86_64", "-static", "-T", str(ROOT / "runtime/guest/guest.ld"), "-o", str(image),
         "@" + str(response), "--wrap=sub_08001494", "--wrap=sub_08001538", "-z", "noexecstack"], "image link")

    # 5. the host
    sdl = next((Path(h) / "x86_64-w64-mingw32" for h in link.SDL_HINTS if h and (Path(h) / "x86_64-w64-mingw32/lib").is_dir()), None)
    if sdl is None:
        sys.exit("SDL3 development files not found: set SHININGSOURCE_SDL3")
    host = out / ("%s_win64.exe" % args.game)
    run([cc, "--target=x86_64-w64-mingw32", "-O2", "-g", "-Wall", "-I", str(ROOT / "runtime/include"), "-I", str(sdl / "include"),
         str(ROOT / "runtime/host64/host_main.c"), str(ROOT / "runtime/win32/plat_sdl.c"), "-o", str(host),
         "-L", str(sdl / "lib"), "-lSDL3", "-lcomdlg32",
         # host code must be callable through 32-bit addresses
         "-Wl,--image-base=0x01800000", "-Wl,--disable-dynamicbase", "-Wl,--stack,0x800000"], "host")
    shutil.copy2(sdl / "bin/SDL3.dll", out / "SDL3.dll")
    print("%s and %s: %d absolute symbols, %d functions still stubbed" % (host.relative_to(ROOT), image.name, len(absolute), len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
