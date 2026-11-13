#!/usr/bin/env python3
"""Build the PS Vita package: build/vita/ShiningSource.vpk (alpha: it has never run on a console).

  build_vita.py [--game ss2] [-j N] [guest|package|all]

The Vita runs 32-bit ARM code but its user memory starts at 0x81000000: the GBA's addresses cannot be mapped.
The game's memory is rebased (tools/rebase_ir.py, proved on a PC by tools/build_rebased.py):

  guest     here, with clang for the Cortex-A9 (ARMv7, Thumb-2, hard float): the game, the runtime core and the
            image's own small C library (runtime/guest) become one object, build/vita/<game>/guest.o, whose only
            visible names are ss_guest_entry and ss_guest_rom;
  package   in the consoles build machine (tools/console_vm.py: VitaSDK and its SDL2): runtime/console/vita_host.c
            is compiled, linked with the guest and packed with the port's own icon (tools/make_icon.py). Nothing
            of the game is in the package: the user copies their ROM to ux0:data/ShiningSource/.
"""
import argparse, os, shutil, struct, subprocess, sys, zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build, link, make_icon, rebase_ir, rebased

ROOT = build.ROOT
TARGET = ["--target=armv7a-none-eabihf", "-mcpu=cortex-a9", "-mfpu=neon", "-mthumb", "-ffreestanding", "-fno-short-enums",
          "-fno-stack-protector", "-fno-common", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables"]
TITLE_ID, VERSION = "SSRC00001", "00.84"
# the Vita's user memory starts at 0x81000000: any address below this is nothing of the program's
STRAY = rebase_ir.STRAY_BELOW(0x10000000)

VM_SCRIPT = r"""set -e
export VITASDK=/root/consoles/vitasdk PATH=/root/consoles/vitasdk/bin:$PATH
cd /root/share
FLAGS="-mcpu=cortex-a9 -mfpu=neon -mthumb -O2 -Wall"
[ -f $VITASDK/arm-vita-eabi/lib/libSDL2.a ] || { echo "VitaSDK has no SDL2 (vdpm sdl2)"; exit 1; }
LIBS="-lSDL2 -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceAudio_stub -lSceAudioIn_stub -lScePower_stub -lSceAppMgr_stub -lSceAppUtil_stub -lSceSysmem_stub -lSceSysmodule_stub -lSceIme_stub -lSceHid_stub -lSceTouch_stub -lSceMotion_stub -lSceCommonDialog_stub -lSceKernelDmacMgr_stub -lm"
arm-vita-eabi-gcc $FLAGS -Isrc/runtime/console -c src/runtime/console/vita_host.c -o host.o
arm-vita-eabi-gcc $FLAGS -Wl,-q host.o guest.o $LIBS -o eboot.elf
arm-vita-eabi-nm -u eboot.elf > undefined.txt || true
# vita-elf-create appends its data to the code segment; when there is no room, 8 KiB of padding moves the end
if ! vita-elf-create eboot.elf eboot.velf > create.log 2>&1; then
  echo 'const unsigned char ss_vita_pad[8192] __attribute__((used)) = { 1 };' > pad.c
  arm-vita-eabi-gcc $FLAGS -c pad.c -o pad.o
  arm-vita-eabi-gcc $FLAGS -Wl,-q host.o guest.o pad.o $LIBS -o eboot.elf
  vita-elf-create eboot.elf eboot.velf > create.log 2>&1
fi
cat create.log | tail -5
vita-make-fself -s -c eboot.velf eboot.bin
vita-mksfoex -s TITLE_ID=@TITLE@ -s APP_VER=@VERSION@ -s VERSION=@VERSION@ -d ATTRIBUTE2=12 "ShiningSource" param.sfo
vita-pack-vpk -s param.sfo -b eboot.bin -a pkg/sce_sys/icon0.png=sce_sys/icon0.png -a pkg/sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png -a pkg/sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png -a pkg/sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml ShiningSource.vpk
# checks: the package's list, the sizes of the program, names the console's libraries do not provide
unzip -l ShiningSource.vpk > check.txt
arm-vita-eabi-size eboot.elf >> check.txt
arm-vita-eabi-readelf -lW eboot.elf | grep -E "LOAD|Entry" >> check.txt
echo "undefined names in the linked program: $(wc -l < undefined.txt)" >> check.txt
arm-vita-eabi-nm eboot.elf | grep -E " (ss_arena|ss_delta|ss_guest_entry|main)$" >> check.txt || true
cat check.txt
ls -l ShiningSource.vpk
"""


def indexed_png(pixels, width, height):
    """An 8-bit palette PNG, as the Vita wants its package pictures: 6 x 7 x 6 levels of colour."""
    levels = lambda value, count: min(count - 1, value * count // 256)
    palette = bytes(c for r in range(6) for g in range(7) for b in range(6) for c in (r * 255 // 5, g * 255 // 6, b * 255 // 5))
    rows = b"".join(b"\0" + bytes(levels(r, 6) * 42 + levels(g, 7) * 6 + levels(b, 6) for r, g, b in pixels[y * width:(y + 1) * width])
                    for y in range(height))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)) + chunk(b"PLTE", palette)
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def picture(width, height, jewel):
    """The port's jewel (tools/make_icon.py), `jewel` pixels across, centred on the icon's dark teal."""
    back = (10, 28, 38)
    icon = make_icon.draw(jewel)
    pixels = [back] * (width * height)
    left, top = (width - jewel) // 2, (height - jewel) // 2
    for y in range(jewel):
        for x in range(jewel):
            r, g, b, a = icon[y * jewel + x]
            pixels[(top + y) * width + left + x] = tuple((c * a + k * (255 - a)) // 255 for c, k in zip((r, g, b), back))
    return indexed_png(pixels, width, height)


def run(cmd, what):
    done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if done.returncode:
        print((done.stdout + done.stderr)[-3000:])
        sys.exit("failed: " + what)
    return done.stdout


def guest(game, jobs):
    cc = build.clang()
    tools = Path(cc).parent
    out = ROOT / "build/vita" / game
    out.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output([cc, "-print-resource-dir"], text=True).strip() + "/include"
    own = ["-nostdinc", "-isystem", resource, "-I", str(ROOT / "runtime/guest/include"), "-I", str(ROOT / "runtime/include"),
           "-I", str(ROOT / "runtime/guest"), "-I", str(ROOT / "runtime/console"), "-DSS_REBASED=1"]
    gen = build.prepare(game, prefix=False)
    replaced = build.port_config(game)["runtime"]
    game_flags = [f for f in build.CFLAGS if not f.startswith("--target") and f not in ("-g", "-mno-ms-bitfields")]
    game_flags += ["-iquote", str(gen / "include"), "-I", str(gen / "include"), *own]
    rt_flags = [*build.RUNTIME_DEFINES, "-O2", "-Wall", *own]
    units = [(p, game_flags, gen, "game") for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced]
    units += [(p, rt_flags, None, "core") for p in sorted((ROOT / "runtime/core").glob("*.c"))]
    units += [(p, rt_flags, None, "plain") for p in (ROOT / "runtime/guest/guest.c", ROOT / "runtime/guest/libc.c",
                                                     ROOT / "runtime/console/rebase.c", ROOT / "runtime/console/co_arm.S")]
    objects = rebased.compile_units(cc, units, out, TARGET, STRAY, rebased.resolver(game), jobs, prefix=False)
    stubs = rebased.tables(game, out, objects, prefix=False)
    for source in ("stubs.c", "codetable.c"):
        target = out / "obj" / (source[:-2] + ".o")
        run([cc, *TARGET, "-O1", "-w", *own, "-c", out / source, "-o", target], source)
        objects.append(target)
    # the link driver's entry points are taken by the core (the other builds use the linker's --wrap): the object
    # that defines one calls it __real_, every other object reaches __wrap_
    for target in objects:
        defined = {l.split()[-1] for l in link.nm(["--defined-only", str(target)]).splitlines() if len(l.split()) == 3}
        renames = ["--redefine-sym=%s=__%s_%s" % (n, "real" if n in defined else "wrap", n) for n in link.WRAPS]
        run([tools / "llvm-objcopy.exe", *renames, target], "wrap " + target.name)
    response = out / "link.rsp"
    response.write_text("\n".join('"%s"' % str(p).replace("\\", "/") for p in objects), encoding="utf-8")
    whole = out / "guest-all.o"
    run([tools / "ld.lld.exe", "-r", "-o", whole, "@" + str(response)], "guest link")
    run([tools / "llvm-objcopy.exe", "-G", "ss_guest_entry", "-G", "ss_guest_rom", whole, out / "guest.o"], "guest names")
    undefined = sorted({l.split()[-1] for l in link.nm(["--undefined-only", str(out / "guest.o")]).splitlines() if l.split()})
    (out / "guest-needs.txt").write_text("\n".join(undefined) + "\n", encoding="utf-8")
    print("%s: %d functions still stubbed; it needs from the console's libraries: %s"
          % ((out / "guest.o").relative_to(ROOT), len(stubs), " ".join(undefined)))


def package(game):
    out = ROOT / "build/vita" / game
    share = out / "vm-share"
    shutil.rmtree(share, ignore_errors=True)
    contents = share / "pkg/sce_sys/livearea/contents"
    contents.mkdir(parents=True)
    (share / "pkg/sce_sys/icon0.png").write_bytes(picture(128, 128, 128))
    (contents / "bg.png").write_bytes(picture(840, 500, 256))
    (contents / "startup.png").write_bytes(picture(280, 158, 128))
    (contents / "template.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<livearea style="a1" format-ver="01.00" content-rev="1">\n'
        '  <livearea-background><image>bg.png</image></livearea-background>\n'
        '  <gate><startup-image>startup.png</startup-image></gate>\n</livearea>\n', encoding="utf-8")
    shutil.copy2(out / "guest.o", share / "guest.o")
    for source in ("runtime/console/vita_host.c", "runtime/guest/bridge.h"):
        (share / "src" / source).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / source, share / "src" / source)
    (share / "build.sh").write_bytes(VM_SCRIPT.replace("@TITLE@", TITLE_ID).replace("@VERSION@", VERSION).encode())
    fetched = out / "vm-out"
    shutil.rmtree(fetched, ignore_errors=True)
    done = subprocess.run([sys.executable, str(ROOT / "tools/console_vm.py"), "--share", str(share), "--run", "bash /root/share/build.sh",
                           "--fetch", "/root/share/ShiningSource.vpk", "--fetch", "/root/share/check.txt", "--fetch", "/root/share/undefined.txt",
                           "--out", str(fetched)], env=dict(os.environ, PYTHONUTF8="1", MSYS_NO_PATHCONV="1"))
    vpk = fetched / "ShiningSource.vpk"
    if done.returncode or not vpk.is_file():
        sys.exit("the build machine did not return the package: see its output above")
    shutil.copy2(vpk, ROOT / "build/vita/ShiningSource.vpk")
    print("build/vita/ShiningSource.vpk (%d bytes); the machine's checks: %s" % (vpk.stat().st_size, (fetched / "check.txt").relative_to(ROOT)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", nargs="?", default="all", choices=["guest", "package", "all"])
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    if args.step in ("guest", "all"):
        guest(args.game, args.j)
    if args.step in ("package", "all"):
        package(args.game)
    return 0


if __name__ == "__main__":
    sys.exit(main())
