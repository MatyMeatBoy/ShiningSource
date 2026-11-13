#!/usr/bin/env python3
"""Build the game image for 64-bit ARM (Android, Linux on ARM; later Switch).

  build_arm64.py [--game ss2] [-j N]

Output in build/arm64/<game>/:
  ss2_game.macho   the game: AArch64 code with 32-bit pointers, linked at 0x0A000000
  ss2_linux_arm64  a test host for Linux on ARM without window or sound (if the ARM64 Linux sysroot is present)

No ELF toolchain offers 32-bit pointers on AArch64; clang's arm64_32 target does, and its object format is
Mach-O. Nothing of Apple's is used: the image is a flat file the host maps and enters, like the x86-64 one.
The game sources are the same generated sources as the other 64-bit builds, with Mach-O's symbol prefix.
"""
import argparse, concurrent.futures, json, os, re, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build, link

ROOT = build.ROOT
BASE = 0x0A000000
# cortex-a53: every 64-bit phone and the Switch run it; without it clang assumes Apple's newer cores
GUEST_TARGET = ["--target=arm64_32-apple-watchos", "-mcpu=cortex-a53", "-ffreestanding", "-fno-stack-protector", "-fno-common",
                "-fno-asynchronous-unwind-tables", "-fno-unwind-tables"]
SYSROOT_HINTS = [os.environ.get("SHININGSOURCE_LINUX_ARM64"), "D:/01Claude/ReKHCoM-build/linux/arm64"]


def run(cmd, what):
    done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if done.returncode:
        print(done.stderr[-3000:])
        sys.exit("failed: " + what)
    return done.stdout


def strip(name):
    return name[1:] if name.startswith("_") else name


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    cc = build.clang()
    bindir = Path(cc).parent
    out = ROOT / "build/arm64" / args.game
    for sub in ("obj", "rt"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    # 1. game modules
    game_flags = [f for f in build.CFLAGS if not f.startswith("--target") and f != "-mno-ms-bitfields"] + GUEST_TARGET
    gen = build.prepare(args.game, prefix=False, abi_flags=game_flags, variant="gen-macho", prefix_after=True, wraps=link.WRAPS)
    replaced = build.port_config(args.game)["runtime"]
    sources = [p for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced]

    # ARM64 does not trap on a division by zero and its remainder then is the dividend; the cartridge's is 0.
    # Each module goes to LLVM IR before any optimisation, the remainders are guarded there, then it is compiled.
    import division_ir
    (out / "ir").mkdir(exist_ok=True)

    def compile_game(path):
        target = out / "obj" / (path.stem + ".o")
        ir = out / "ir" / (path.stem + ".ll")
        done = subprocess.run([cc, *game_flags, "-iquote", str(gen / "include"), "-I", str(gen / "include"),
                               "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm", str(path), "-o", str(ir)],
                              capture_output=True, text=True, cwd=gen)
        if done.returncode:
            return path.stem, done.returncode, done.stderr
        text, guarded = division_ir.lower(ir.read_text(encoding="utf-8"), "rem")
        ir.write_text(text, encoding="utf-8")
        optimisation = [f for f in game_flags if f.startswith("-O") or f.startswith("--target") or f.startswith("-mcpu")]
        done = subprocess.run([cc, *optimisation, "-w", "-c", str(ir), "-o", str(target)], capture_output=True, text=True)
        return path.stem, done.returncode, done.stderr
    failed = []
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        for name, code, errors in pool.map(compile_game, sources):
            if code:
                failed.append((name, errors))
    if failed:
        for name, errors in failed[:3]:
            print(name, "\n", "\n".join(l for l in errors.splitlines() if "error:" in l)[:1500])
        sys.exit("%d game module(s) do not compile for the ARM64 image" % len(failed))
    objects = sorted((out / "obj").glob("*.o"))
    nm = lambda flags, files: link.nm([*flags, *map(str, files)])

    # 2. runtime core and the image's platform layer
    runtime_objects = []
    rt_flags = [*GUEST_TARGET, "-O2", "-g", "-Wall", "-I", str(ROOT / "runtime/guest/include"), "-I", str(ROOT / "runtime/include"),
                "-I", str(ROOT / "runtime/guest")]
    rt_flags += ["-D%s=__wrap_%s" % (name, name) for name in link.WRAPS]     # the runtime's own calls are wrapped too
    for source in sorted((ROOT / "runtime/core").glob("*.c")) + sorted((ROOT / "runtime/guest").glob("*.c")) + [ROOT / "runtime/guest/co_a64.S"]:
        target = out / "rt" / (source.stem + ".o")
        run([cc, *rt_flags, "-c", source, "-o", target], source.name)
        runtime_objects.append(target)

    # 3. what the game still needs: absolute symbols, stubs, the code-pointer table (same rules as the other builds)
    defined = {strip(l.split()[-1]) for l in nm(["--defined-only"], objects + runtime_objects).splitlines() if len(l.split()) == 3}
    undefined = lambda files: {strip(l.split()[-1]) for l in nm(["--undefined-only"], files).splitlines() if len(l.split()) in (1, 2) and not l.rstrip().endswith(":")}
    named, functions = link.named_addresses(args.game), link.function_names(args.game)
    wanted = (undefined(objects) | {n for n in undefined(runtime_objects) if link.ADDRESS.match(n) or n in named or n in functions}) - defined
    wanted -= set(link.WRAPS)       # reached under their __wrap_ names, see the code table below
    absolute, stubs, unknown = {}, [], []
    for name in sorted(wanted):
        m = link.ADDRESS.match(name)
        if m:
            absolute[name] = int(m[1], 16) + int(m[2] or 0)
        elif name in named:
            absolute[name] = named[name]
        elif name in functions or re.fullmatch(r"sub_08[0-9A-F]{6}", name):
            stubs.append(name)
        elif name.startswith("__real_") or name.startswith("__wrap_") or name.startswith("section$"):
            continue
        else:
            unknown.append(name)
    if unknown:
        sys.exit("symbols that fit no group:\n  " + "\n  ".join(unknown))
    (out / "absolute.s").write_text("\n".join('\t.globl "_%s"\n"_%s" = 0x%08X' % (n, n, a) for n, a in sorted(absolute.items())) + "\n",
                                    encoding="utf-8")
    (out / "stubs.c").write_text('#include "shiningsource/runtime.h"\n' + "\n".join(
        "int %s(void) { static int seen; ss_stub(\"%s\", &seen); return 0; }" % (n, n) for n in stubs) + "\n", encoding="utf-8")
    entries = []
    for line in (build.GAMES[args.game] / "tools/config/functions.cfg").read_text(encoding="utf-8").splitlines():
        fields = line.split("#")[0].split()
        if len(fields) >= 2:
            address = int(fields[1], 16) & ~1
            name = fields[2] if len(fields) > 2 else "sub_%08X" % address
            if name in link.WRAPS:
                name = "__wrap_" + name
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
        run([cc, *rt_flags, "-w", "-c", out / source, "-o", target], source)
        extra.append(target)

    # 4. link the image: no page zero below it but the range up to its base, no chained fixups (the data then
    #    holds plain addresses and the host has nothing to relocate)
    image = out / ("%s_game.macho" % args.game)
    response = out / "link.rsp"
    response.write_text("\n".join('"%s"' % str(p).replace("\\", "/") for p in objects + runtime_objects + extra), encoding="utf-8")
    run([bindir / "ld.lld.exe", "-flavor", "darwin", "-arch", "arm64_32", "-platform_version", "watchos", "9.0", "9.0",
         "-e", "_ss_guest_entry", "-pagezero_size", hex(BASE), "-no_fixup_chains", "-map", out / "image.map", "-o", image,
         "@" + str(response)], "image link")
    print("%s: %d absolute symbols, %d functions still stubbed" % (image.relative_to(ROOT), len(absolute), len(stubs)))

    # 5. test host for Linux on ARM (headless)
    base = next((Path(h) for h in SYSROOT_HINTS if h and (Path(h) / "sysroot").is_dir()), None)
    if base is None:
        print("no ARM64 Linux sysroot (SHININGSOURCE_LINUX_ARM64): test host skipped")
        return 0
    host = out / ("%s_linux_arm64" % args.game)
    run([cc, "--target=aarch64-linux-gnu", "--sysroot=" + str(base / "sysroot"), "-fuse-ld=lld", "-O2", "-g", "-Wall",
         "-I", ROOT / "runtime/include", ROOT / "runtime/host64/host_linux.c", ROOT / "runtime/host64/plat_null.c",
         ROOT / "runtime/plat/net_udp.c", "-o", host, "-lm"], "ARM64 Linux test host")
    print("%s (cross-compiled; run it on ARM64 Linux)" % host.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
