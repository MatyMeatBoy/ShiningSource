#!/usr/bin/env python3
"""Build the web version (WebAssembly, Emscripten): build/web/<game>/index.html, .js and .wasm.

  build_web.py [--game ss2] [-j N]

Needs Emscripten (SHININGSOURCE_EMSDK, default D:/01Claude/ReKHCoM-build/toolchain/emsdk), a static SDL3 built
for it (SHININGSOURCE_SDL3_WEB: folder with libSDL3.a) and the SDL3 headers (SHININGSOURCE_SDL3_INCLUDE).
No ROM is packaged: the page asks the user for their file.

WebAssembly is stricter than the processors the other builds target, and every module therefore goes through
LLVM IR before it is compiled:
  - the game's variables at fixed addresses become constants (tools/absolute_ir.py): the linker cannot place
    a symbol at an address;
  - every division is given the cartridge's results explicitly (tools/division_ir.py): a division by zero
    stops a WebAssembly program for good;
  - a function declared with another signature than its definition is redeclared with the real one, and the
    compiler then adapts each call (a call with a signature that is not the function's is replaced by a trap
    otherwise). The decompilation's declarations are being corrected at the source; this keeps the build
    independent of how far that is.
"""
import argparse, concurrent.futures, json, os, re, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import absolute_ir, build, division_ir, link

ROOT = build.ROOT
DEFINE = re.compile(r'^define (?:[\w()]+ )*?((?:[\w.%"*\[\] ]+?)) @([\w.$]+)\(((?:[^()\n]|\([^()\n]*\))*)\)[^\n]*\{', re.M)
DECLARE = re.compile(r'^declare (?:(?:![\w.]+|[\w()]+) )*?((?:[\w.%"*\[\] ]+?)) @([\w.$]+)\(((?:[^()\n]|\([^()\n]*\))*)\)[^\n]*$', re.M)


def plain(parameters):
    """Parameter list of a definition without the value names: what a declaration of it looks like."""
    out, depth, current = [], 0, ""
    for ch in parameters + ",":
        depth += ch in "([{"
        depth -= ch in ")]}"
        if ch == "," and depth == 0:
            text = re.sub(r'\s+%[\w.]+\s*$', "", current.strip())
            if text:
                out.append(text)
            current = ""
        else:
            current += ch
    return ", ".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    emsdk = Path(os.environ.get("SHININGSOURCE_EMSDK", "D:/01Claude/ReKHCoM-build/toolchain/emsdk"))
    emcc = emsdk / "upstream/emscripten/emcc.bat"
    if not emcc.exists():
        emcc = emsdk / "upstream/emscripten/emcc.exe"
    nm = emsdk / "upstream/bin/llvm-nm.exe"
    sdl_lib = Path(os.environ.get("SHININGSOURCE_SDL3_WEB", "D:/01Claude/ReKHCoM-build/web-sdl"))
    sdl_include = Path(os.environ.get("SHININGSOURCE_SDL3_INCLUDE", "D:/01Claude/ReKHCoM-build/toolchain/android/SDL3-3.4.18/include"))
    for needed in (emcc, nm, sdl_lib / "libSDL3.a", sdl_include / "SDL3/SDL.h"):
        if not needed.exists():
            sys.exit("missing: %s" % needed)
    env = dict(os.environ)          # emcc finds its own configuration next to itself
    out = ROOT / "build/web" / args.game
    for sub in ("ir", "obj"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    def run(cmd, what, cwd=None):
        done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env, cwd=cwd)
        if done.returncode:
            return what + "\n" + (done.stdout + done.stderr)[-2500:]
        return None

    game_flags = [f for f in build.CFLAGS if not f.startswith("--target") and f != "-mno-ms-bitfields"]
    # struct sizes are measured with the 32-bit Windows target: same sizes, and that compiler has no WebAssembly
    gen = build.prepare(args.game, prefix=False, abi_flags=build.CFLAGS, variant="gen-web")
    replaced = build.port_config(args.game)["runtime"]
    units = [(p, game_flags + ["-iquote", str(gen / "include"), "-I", str(gen / "include")], gen)
             for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced]
    rt_flags = ["-O2", "-g0", "-Wall", "-I", str(ROOT / "runtime/include"), "-I", str(sdl_include)]
    skipped = {"netlink", "debug_alloc"}        # no sockets in a browser; the allocator diagnostics rely on a linker wrap
    runtime = [p for p in sorted((ROOT / "runtime/core").glob("*.c")) if p.stem not in skipped]
    runtime += [ROOT / "runtime/web/web_main.c", ROOT / "runtime/win32/plat_sdl.c"]
    units += [(p, rt_flags, None) for p in runtime]

    # 1. everything to IR, before any optimisation
    def to_ir(unit):
        path, flags, cwd = unit
        ir = out / "ir" / (path.stem + ".ll")
        return run([emcc, str(path), *flags, "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm", "-o", str(ir)], path.name, cwd)
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        errors = [e for e in pool.map(to_ir, units) if e]
    if errors:
        print("\n".join(errors[:3]))
        sys.exit("%d file(s) did not compile to IR" % len(errors))

    # 2. the real signature of every function defined in the game
    texts = {u[0].stem: (out / "ir" / (u[0].stem + ".ll")).read_text(encoding="utf-8") for u in units}
    signatures = {}
    for text in texts.values():
        for m in DEFINE.finditer(text):
            signatures[m[2]] = (m[1].strip(), plain(m[3]))
    # the C library's own, which some modules declare as returning nothing
    signatures["memcpy"] = ("ptr", "ptr, ptr, i32")
    signatures["memset"] = ("ptr", "ptr, i32, i32")
    named = link.named_addresses(args.game)

    def resolve(name):
        m = link.ADDRESS.match(name)
        if m:
            return int(m[1], 16) + int(m[2] or 0)
        return named.get(name)

    # 3. rewrite and compile
    counts = {"addresses": 0, "divisions": 0, "declarations": 0}

    def finish(unit):
        path = unit[0]
        text = texts[path.stem]
        text, n = absolute_ir.lower(text, resolve)
        counts["addresses"] += n
        text, n = division_ir.lower(text, "all")
        counts["divisions"] += n

        def redeclare(m):
            real = signatures.get(m[2])
            if not real or (real[0] == m[1].strip() and real[1] == plain(m[3])):
                return m[0]
            counts["declarations"] += 1
            # the struct's type may not exist in this module; each call says itself what it passes by value
            return "declare %s @%s(%s)" % (real[0], m[2], re.sub(r'\s*byval\(%[^)]*\)', "", real[1]))
        text = DECLARE.sub(redeclare, text)
        ir = out / "ir" / (path.stem + ".ll")
        ir.write_text(text, encoding="utf-8")
        level = "-O1" if unit[2] else "-O2"
        return run([emcc, level, "-w", "-c", str(ir), "-o", str(out / "obj" / (path.stem + ".o"))], path.name + " (from IR)")
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        errors = [e for e in pool.map(finish, units) if e]
    if errors:
        print("\n".join(errors[:3]))
        sys.exit("%d file(s) did not compile from IR" % len(errors))
    objects = [out / "obj" / (u[0].stem + ".o") for u in units]

    # 4. what the game still needs: stubs for functions nobody implements, the code-pointer table
    listing = lambda flags: subprocess.run([str(nm), *flags, *map(str, objects)], capture_output=True, text=True).stdout.splitlines()
    defined = {l.split()[-1] for l in listing(["--defined-only"]) if len(l.split()) == 3}
    undefined = {l.split()[-1] for l in listing(["--undefined-only"]) if len(l.split()) == 2 and l.split()[0] == "U"}
    functions = link.function_names(args.game)
    stubs = sorted(n for n in undefined - defined if n in functions or re.fullmatch(r"sub_08[0-9A-F]{6}", n))
    stubs = [n for n in stubs if not n.startswith("__") and n not in ("memcpy", "memset")]       # the cartridge's arithmetic helpers are in runtime/web
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
    table += ["void %s();" % n for n in sorted({n for _, n in entries})]
    table += ["const struct ss_code_entry ss_code_table[] = {"] + ["    { 0x%08Xu, (void (*)(void))%s }," % e for e in entries] + ["};"]
    table += ["const unsigned ss_code_count = %d;" % len(entries),
              "const uint32_t ss_code_ranges[][2] = { %s };" % ", ".join("{ %s, %s }" % (a, b) for a, b in ranges),
              "const unsigned ss_code_range_count = %d;" % len(ranges)]
    (out / "codetable.c").write_text("\n".join(table) + "\n", encoding="utf-8")
    for source in ("stubs.c", "codetable.c"):
        error = run([emcc, "-O1", "-w", "-I", str(ROOT / "runtime/include"), "-c", str(out / source), "-o", str(out / "obj" / (source[:-2] + ".o"))], source)
        if error:
            sys.exit(error)
        objects.append(out / "obj" / (source[:-2] + ".o"))

    # 5. link: the program's own memory starts above the GBA's address space
    response = out / "link.rsp"
    response.write_text("\n".join('"%s"' % str(p).replace("\\", "/") for p in objects) + '\n"%s"\n' % str(sdl_lib / "libSDL3.a").replace("\\", "/"),
                        encoding="utf-8")
    error = run([emcc, "-O2", "@" + str(response), "-o", str(out / "index.html"), "--shell-file", str(ROOT / "runtime/web/shell.html"),
                 "-sASYNCIFY=1", "-sASYNCIFY_STACK_SIZE=131072", "-sEMULATE_FUNCTION_POINTER_CASTS=1",
                 "-sGLOBAL_BASE=167772160", "-sINITIAL_MEMORY=268435456", "-sSTACK_SIZE=1048576",
                 "-sFORCE_FILESYSTEM=1", "-sENVIRONMENT=web", "-sEXPORTED_RUNTIME_METHODS=[\"FS\"]"], "link")
    if error:
        (out / "link.log").write_text(error, encoding="utf-8")
        lines = [l for l in error.splitlines() if "error" in l or "mismatch" in l or "undefined" in l]
        print("\n".join(l[-160:] for l in lines[:25]))
        sys.exit("link failed (full output in %s)" % (out / "link.log").relative_to(ROOT))
    print("%s: %d fixed addresses as constants, %d divisions guarded, %d declarations given the real signature, %d functions still stubbed"
          % ((out / "index.html").relative_to(ROOT), counts["addresses"], counts["divisions"], counts["declarations"], len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
