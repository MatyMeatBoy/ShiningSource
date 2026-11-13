#!/usr/bin/env python3
"""Build the PC check of the consoles' memory scheme: build/rebased/<game>/<game>_rebased.exe.

  build_rebased.py [--game ss2] [-j N]

The Switch and the Vita cannot give the game its memory at the GBA's addresses; tools/rebase_ir.py moves it
into a block of the program. This builds the development program the same way (32-bit Windows, the same
sources, the same platform layer) so that the scheme can be run against the reference on a PC:

  SS_SKIP=0 SS_FRAMES=32000 SS_INPUT_FILE=input_play.txt build/rebased/ss2/ss2_rebased.exe rom.gba

must print the same `frame 32000  hash` and `audio:` lines as the development build. In this program the GBA's
own addresses are closed (runtime/console/check_win32.c) and no fault is absorbed.
"""
import argparse, os, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build, link, rebase_ir, rebased

ROOT = build.ROOT
# What this PC program takes for an address the console would have ignored: the null page, and the 8 MiB below
# its own image (linked at 0x01800000), where the cartridge's one bad pointer lies. The consoles' test is
# simpler (everything below their memory): their own memory is far above.
STRAY = rebase_ir.STRAY_RANGES([(0, 0x10000), (0x01000000, 0x01800000)])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    cc = build.clang()
    out = ROOT / "build/rebased" / args.game
    out.mkdir(parents=True, exist_ok=True)
    sdl = next((Path(h) / "i686-w64-mingw32" for h in link.SDL_HINTS if h and (Path(h) / "i686-w64-mingw32/lib").is_dir()), None)
    if sdl is None:
        sys.exit("SDL3 development files not found: set SHININGSOURCE_SDL3 to the SDL3 mingw package folder")
    codegen = ["--target=i686-w64-mingw32"]
    gen = build.prepare(args.game)
    replaced = build.port_config(args.game)["runtime"]
    game_flags = [f for f in build.CFLAGS if not f.startswith("--target") and f != "-g"] + ["-iquote", str(gen / "include"), "-I", str(gen / "include"),
                                                                                           "-I", str(ROOT / "runtime/include")]
    rt_flags = [*build.RUNTIME_DEFINES, "-O2", "-Wall", "-I", str(ROOT / "runtime/include"), "-I", str(sdl / "include")]
    units = [(p, game_flags, gen, "game") for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced]
    units += [(p, rt_flags, None, "core") for p in sorted((ROOT / "runtime/core").glob("*.c")) + [ROOT / "runtime/win32/plat_sdl.c"]]
    # the block comes last: the range kept closed must be the first large thing in the image
    units += [(p, rt_flags, None, "plain") for p in (ROOT / "runtime/console/check_win32.c", ROOT / "runtime/plat/net_udp.c",
                                                     ROOT / "runtime/console/rebase.c")]
    objects = rebased.compile_units(cc, units, out, codegen, STRAY, rebased.resolver(args.game), args.j, prefix=True)
    stubs = rebased.tables(args.game, out, objects, prefix=True)
    for source in ("stubs.c", "codetable.c"):
        target = out / "obj" / (source[:-2] + ".o")
        done = subprocess.run([cc, *codegen, "-O1", "-w", "-I", str(ROOT / "runtime/include"), "-c", str(out / source), "-o", str(target)],
                              capture_output=True, text=True)
        if done.returncode:
            sys.exit(done.stderr[:4000])
        objects.insert(-1, target)
    exe = out / ("%s_rebased.exe" % args.game)
    response = out / "link.rsp"
    response.write_text("\n".join('"%s"' % str(p).replace("\\", "/") for p in objects), encoding="utf-8")
    done = subprocess.run([cc, *codegen, "@" + str(response), "-o", str(exe), "-Wl,--image-base=0x01800000", "-Wl,--disable-dynamicbase",
                           "-Wl,--stack,0x400000", "-L", str(sdl / "lib"), "-lSDL3", "-Wl,--delayload=SDL3.dll", "-lws2_32",
                           *["-Wl,--wrap=" + name for name in link.WRAPS]], capture_output=True, text=True)
    if done.returncode:
        print("\n".join(done.stderr.splitlines()[:40]))
        sys.exit("link failed")
    shutil.copy2(sdl / "bin/SDL3.dll", out / "SDL3.dll")
    print("%s: %d functions still stubbed" % (exe.relative_to(ROOT), len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
