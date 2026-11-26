#!/usr/bin/env python3
"""Build the 64-bit Linux version by cross-compiling from Windows.

  build_linux64.py [--game ss2]

The game image uses the same relocatable/rebased x32 pipeline as Android; this script
builds it, then compiles the Linux host against a Debian sysroot and an SDL3 built for it.

  SHININGSOURCE_LINUX_SYSROOT   folder with `sysroot/` and `sdl/{include,lib}` (default: the one prepared for
                                ReKHCoM, D:/01Claude/ReKHCoM-build/linux/amd64)

Output: build/linux64/<game>/{ss2_linux64, ss2_game.elf, lib/libSDL3.so.0}.
Run on Linux with: ./ss2_linux64 ROM.gba. Original GBA addresses need not be available.
RetroAchievements: SS_RA_DIR=<that folder> evaluates the shipped set (runtime/achievements/ach_host.c); a sign-in
(retroachievements.auth, platform/ga36/ra_vincular_cuenta.py) with SS_RA_SUBMIT=1 sends unlocks over the system's OpenSSL.
"""
import argparse, os, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build
import ra_build
from launcher_art import SIDE_ART

ROOT = build.ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    args = parser.parse_args()
    base = Path(os.environ.get("SHININGSOURCE_LINUX_SYSROOT", "D:/01Claude/ReKHCoM-build/linux/amd64"))
    if not (base / "sysroot").is_dir() or not (base / "sdl/lib").is_dir():
        sys.exit("Linux sysroot not found: set SHININGSOURCE_LINUX_SYSROOT")
    done = subprocess.run([sys.executable, str(ROOT / "tools/build_android_image.py"), "--game", args.game, "--abi", "x86_64", "-j", "4"])
    if done.returncode:
        sys.exit("the relocatable game image did not build")
    image = ROOT / "build/android/images/x86_64" / args.game / ("%s_game.elf" % args.game)
    out = ROOT / "build/linux64" / args.game
    (out / "lib").mkdir(parents=True, exist_ok=True)
    host = out / ("%s_linux64" % args.game)
    done = subprocess.run([build.clang(), "--target=x86_64-linux-gnu", "--sysroot=" + str(base / "sysroot"), "-fuse-ld=lld",
                           "-DSS_REBASED_HOST=1", "-DSS_HOST_VERSION=\"0.9.5\"",
                           "-O2", "-g", "-Wall", "-I", str(ROOT / "runtime/include"), "-I", str(base / "sdl/include"),
                           *ra_build.flags(handheld=False),
                           str(ROOT / "runtime/host64/host_linux.c"), str(ROOT / "runtime/win32/plat_sdl.c"), str(ROOT / "runtime/plat/net_udp.c"),
                           *[str(p) for p in ra_build.sources()], "-o", str(host),
                           "-L", str(base / "sdl/lib"), "-lSDL3", "-lm", *ra_build.LIBRARIES, "-Wl,-rpath,$ORIGIN/lib"], capture_output=True, text=True)
    if done.returncode:
        print(done.stderr[-3000:])
        sys.exit("the Linux host did not build")
    shutil.copy2(image, out / image.name)
    artwork = Path("assets/launcher")
    (out / artwork).mkdir(parents=True, exist_ok=True)
    for name in SIDE_ART:
        shutil.copy2(ROOT / artwork / name, out / artwork / name)
    for name, source in ra_build.package_files().items():     # RetroAchievements: SS_RA_DIR=<this folder> evaluates them
        (out / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, out / name)
    for library in (base / "sdl/lib").glob("libSDL3.so.0*"):
        if not library.is_symlink():
            shutil.copy2(library, out / "lib/libSDL3.so.0")
    print("%s and %s (cross-compiled; run it on Linux to test)" % (host.relative_to(ROOT), image.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
