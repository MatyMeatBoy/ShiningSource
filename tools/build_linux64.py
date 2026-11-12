#!/usr/bin/env python3
"""Build the 64-bit Linux version by cross-compiling from Windows.

  build_linux64.py [--game ss2]

The game image is the one build_win64.py produces (it is the same file on every operating system); this script
builds it if needed, then compiles the Linux host against a Debian sysroot and an SDL3 built for it.

  SHININGSOURCE_LINUX_SYSROOT   folder with `sysroot/` and `sdl/{include,lib}` (default: the one prepared for
                                ReKHCoM, D:/01Claude/ReKHCoM-build/linux/amd64)

Output: build/linux64/<game>/{ss2_linux64, ss2_game.elf, lib/libSDL3.so.0}.
Run on Linux with: ./ss2_linux64 ROM.gba     (needs vm.mmap_min_addr <= 0x02000000, the default is 65536)
"""
import argparse, os, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build

ROOT = build.ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    args = parser.parse_args()
    base = Path(os.environ.get("SHININGSOURCE_LINUX_SYSROOT", "D:/01Claude/ReKHCoM-build/linux/amd64"))
    if not (base / "sysroot").is_dir() or not (base / "sdl/lib").is_dir():
        sys.exit("Linux sysroot not found: set SHININGSOURCE_LINUX_SYSROOT")
    image = ROOT / "build/win64" / args.game / ("%s_game.elf" % args.game)
    if not image.exists():
        done = subprocess.run([sys.executable, str(ROOT / "tools/build_win64.py"), "--game", args.game])
        if done.returncode:
            sys.exit("the game image did not build")
    out = ROOT / "build/linux64" / args.game
    (out / "lib").mkdir(parents=True, exist_ok=True)
    host = out / ("%s_linux64" % args.game)
    done = subprocess.run([build.clang(), "--target=x86_64-linux-gnu", "--sysroot=" + str(base / "sysroot"), "-fuse-ld=lld",
                           "-no-pie",       # host code must be callable through 32-bit addresses
                           "-O2", "-g", "-Wall", "-I", str(ROOT / "runtime/include"), "-I", str(base / "sdl/include"),
                           str(ROOT / "runtime/host64/host_linux.c"), str(ROOT / "runtime/win32/plat_sdl.c"), str(ROOT / "runtime/plat/net_udp.c"), "-o", str(host),
                           "-L", str(base / "sdl/lib"), "-lSDL3", "-lm", "-Wl,-rpath,$ORIGIN/lib"], capture_output=True, text=True)
    if done.returncode:
        print(done.stderr[-3000:])
        sys.exit("the Linux host did not build")
    shutil.copy2(image, out / image.name)
    for library in (base / "sdl/lib").glob("libSDL3.so.0*"):
        if not library.is_symlink():
            shutil.copy2(library, out / "lib/libSDL3.so.0")
    print("%s and %s (cross-compiled; run it on Linux to test)" % (host.relative_to(ROOT), image.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
