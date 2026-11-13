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
  - a function declared with another signature than its definition is redeclared with the real one, and each
    call that does not agree with it goes through a small function that converts the values as the cartridge's
    registers would have (adapt_calls below): a call with a signature that is not the function's is replaced
    by a trap otherwise. A function another module declares as a variable, to take its address, is declared as
    the function it is: the linker crashes on a name that is a function here and data there. The
    decompilation's declarations are being corrected at the source; this keeps the build independent of how
    far that is.

Emscripten is run with its own Python: its library cache is emptied whenever the spelling of its path changes,
which depends on the Python that runs it.
"""
import argparse, concurrent.futures, json, os, re, shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import absolute_ir, build, division_ir, link

ROOT = build.ROOT
# a variable a module defines and other modules can see (not `external`, not local to the module)
VARIABLE = re.compile(r'^@(?:"((?:[^"\\]|\\.)+)"|([-\w.$]+)) = (?!external |internal |private |extern_weak )[^\n]*\b(?:global|constant) ', re.M)
DEFINE =re.compile(r'^define (?:[\w()]+ )*?((?:[\w.%"*\[\] ]+?)) @([\w.$]+)\(((?:[^()\n]|\([^()\n]*\))*)\)[^\n]*\{', re.M)
DECLARE = re.compile(r'^declare (?:(?:![\w.]+|[\w()]+) )*?((?:[\w.%"*\[\] ]+?)) @([\w.$]+)\(((?:[^()\n]|\([^()\n]*\))*)\)[^\n]*$', re.M)


CALL = re.compile(r'^(\s*(?:%[\w.]+ = )?(?:tail |notail )?call )([^@\n]*?)@([\w.$]+)\(', re.M)
BITS = {"i1": 1, "i8": 8, "i16": 16, "i32": 32, "i64": 64, "ptr": 32}
BYVAL = re.compile(r'byval\([^)]*\)(?: align \d+)?')


def pieces(parameters):
    """A parameter or argument list cut at its commas (not those inside brackets)."""
    out, depth, current = [], 0, ""
    for ch in parameters + ",":
        depth += ch in "([{"
        depth -= ch in ")]}"
        if ch == "," and depth == 0:
            if current.strip():
                out.append(current.strip())
            current = ""
        else:
            current += ch
    return out


def plain(parameters):
    """Parameter list of a definition without the value names: what a declaration of it looks like."""
    return ", ".join(re.sub(r'\s+%[\w.]+\s*$', "", text) for text in pieces(parameters))


def shape(text, result=False):
    """(type, extension, by-value attribute) of a parameter or an argument, or of a return type, as the IR writes it:
    the type comes first in a parameter ("i16 noundef signext %0") and last in a result ("hidden signext i16")."""
    words = text.split()
    extension = "signext" if "signext" in words else "zeroext" if "zeroext" in words else ""
    by_value = BYVAL.search(text)
    return words[-1] if result else words[0], extension, by_value[0] if by_value else ""


def convert(lines, value, source, target, signed):
    """Add the instructions that turn `value` of type `source` into type `target`; returns the new value."""
    def step(operation, a, b, value):
        name = "%%c%d" % len(lines)
        lines.append("  %s = %s %s %s to %s" % (name, operation, a, value, b))
        return name
    if source == target:
        return value
    if source == "ptr":
        value, source = step("ptrtoint", "ptr", "i32", value), "i32"
    integer = "i32" if target == "ptr" else target
    if source != integer:
        value = step("trunc" if BITS[source] > BITS[integer] else "sext" if signed else "zext", source, integer, value)
    return step("inttoptr", "i32", "ptr", value) if target == "ptr" else value


def adapt_calls(text, signatures):
    """Make every direct call agree with the function it calls.

    The decompilation still declares many functions with another signature than their definition has (a 16-bit
    parameter declared as 32-bit, a missing parameter, a result nobody returns). The cartridge's processor does
    not care: parameters travel in registers. WebAssembly checks the signature, and the compiler replaces a call
    it cannot reconcile by a trap. Each such call therefore goes through a small function with the signature the
    call uses, which converts the values as the registers would have: a narrower value is the low bits, a wider
    one is extended as its declaration says, a parameter the call does not give is 0, and so is a result the
    function does not return. Returns (new text, number of calls redirected, calls left alone because of a type
    this does not know).
    """
    local = {m[2]: (m[1].strip(), plain(m[3])) for m in DEFINE.finditer(text)}
    wrappers, unknown = {}, set()

    def redirect(m):
        name = m[3]
        real = local.get(name) or signatures.get(name)
        if not real or "(" in m[2]:                    # not the game's, or a call that states a whole function type
            return m[0]
        end, depth = m.end(), 1
        while depth:
            depth += text[end] == "("
            depth -= text[end] == ")"
            end += 1
        arguments = [shape(a) for a in pieces(text[m.end():end - 1])]
        result, real_result = shape(m[2], True), shape(real[0], True)
        parameters = [shape(p) for p in pieces(real[1])]
        if result[0] == real_result[0] and [a[0] for a in arguments] == [p[0] for p in parameters]:
            return m[0]
        kinds = [a[0] for a in arguments] + [p[0] for p in parameters]
        if any(k not in BITS for k in kinds) or any(k != "void" and k not in BITS for k in (result[0], real_result[0])):
            unknown.add(name)
            return m[0]
        key = (name, result[:2], tuple(arguments))
        if key not in wrappers:
            alias = "%s.as.%d" % (name, len(wrappers))
            lines = ["define internal %s @%s(%s) {" % (" ".join(filter(None, (result[1], result[0]))), alias, ", ".join(
                "%s %%a%d" % (" ".join(filter(None, (a[0], a[1], a[2]))), i) for i, a in enumerate(arguments)))]
            passed = []
            for i, (kind, extension, by_value) in enumerate(parameters):
                if i < len(arguments):
                    value = convert(lines, "%%a%d" % i, arguments[i][0], kind, arguments[i][1] == "signext")
                    by_value = arguments[i][2] if by_value and kind == arguments[i][0] else ""
                else:
                    value, by_value = "null" if kind == "ptr" else "0", ""
                passed.append("%s %s" % (" ".join(filter(None, (kind, extension, by_value))), value))
            call = "call %s @%s(%s)" % (" ".join(filter(None, (real_result[1], real_result[0]))), name, ", ".join(passed))
            if real_result[0] == "void":
                lines.append("  " + call)
                value = "null" if result[0] == "ptr" else "0"
            else:
                lines.append("  %r = " + call)
                value = "%r" if result[0] == "void" else convert(lines, "%r", real_result[0], result[0], real_result[1] == "signext")
            lines += ["  ret void" if result[0] == "void" else "  ret %s %s" % (result[0], value), "}"]
            wrappers[key] = (alias, "\n".join(lines))
        return "%s%s@%s(" % (m[1], m[2], wrappers[key][0])

    new = CALL.sub(redirect, text)
    if wrappers:
        new = new.rstrip("\n") + "\n\n" + "\n\n".join(body for _, body in wrappers.values()) + "\n"
    return new, len(wrappers), unknown


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
    # Emscripten empties its library cache when the spelling of its own path changes, and that spelling depends
    # on which Python runs it (MSYS2's writes "/", the others "\"): always its own
    pythons = sorted((emsdk / "python").glob("*/python.exe"))
    if pythons and "EMSDK_PYTHON" not in env:
        env["EMSDK_PYTHON"] = str(pythons[-1])
    out = ROOT / "build/web" / args.game
    for sub in ("ir", "obj"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    def run(cmd, what, cwd=None, keep=2500):
        done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env, cwd=cwd)
        if done.returncode:
            return what + "\n" + (done.stdout + done.stderr)[-keep:]
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
            if not m[0].startswith(("define internal ", "define private ")):        # a module's own: no one else calls it
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
    counts = {"addresses": 0, "divisions": 0, "declarations": 0, "kinds": 0, "calls": 0}
    unadapted = set()
    # the opposite case has no automatic answer: say so here, the linker would only crash
    variables = {m[1] if m[1] is not None else m[2] for text in texts.values() for m in VARIABLE.finditer(text)}
    confused = sorted(variables & set(signatures))
    confused += sorted(m[2] for text in texts.values() for m in DECLARE.finditer(text) if m[2] in variables and m[2] not in signatures)
    if confused:
        sys.exit("defined as a variable in one module and used as a function in another: " + ", ".join(sorted(set(confused))))

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

        # a function whose address is taken through a variable's declaration (`extern u32 sub_0805990C;`): a
        # WebAssembly function is not data, and the linker crashes on a name that is one here and the other there
        def as_function(m):
            name = m[1] if m[1] is not None else m[2]
            real = signatures.get(name)
            if not real:
                return m[0]
            counts["kinds"] += 1
            if re.search(r'^declare [^\n]*@%s\(' % re.escape(name), text, re.M):
                return ""
            return "declare %s @%s(%s)" % (real[0], name, re.sub(r'\s*byval\(%[^)]*\)', "", real[1]))
        text = absolute_ir.DECLARATION.sub(as_function, text)
        text, n, unknown = adapt_calls(text, signatures)
        counts["calls"] += n
        unadapted.update(unknown)
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
    # the compiler's own name for a call it could not reconcile with the function: such a call stops the program
    traps = sorted({n.split("_bitcast_invalid")[0].removeprefix(".L") for n in defined if "_bitcast_invalid" in n} | unadapted)
    if traps:
        print("warning: calls with a signature that is not the function's remain (they stop the program when reached): "
              + ", ".join(traps[:20]) + (" and %d more" % (len(traps) - 20) if len(traps) > 20 else ""))
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
                 "-sFORCE_FILESYSTEM=1", "-lidbfs.js", "-sENVIRONMENT=web", "-sEXPORTED_RUNTIME_METHODS=[\"FS\"]"], "link",
                keep=200000)            # the linker's messages come before its very long command line
    if error:
        (out / "link.log").write_text(error, encoding="utf-8")
        lines = [l for l in error.splitlines() if "error" in l or "mismatch" in l or "undefined" in l]
        print("\n".join(l[-160:] for l in lines[:25]))
        sys.exit("link failed (full output in %s)" % (out / "link.log").relative_to(ROOT))
    (out / "link.log").unlink(missing_ok=True)
    shutil.copyfile(ROOT / "runtime/web/favicon.png", out / "favicon.png")
    print("%s: %d fixed addresses as constants, %d divisions guarded, %d declarations given the real signature "
          "(%d of a function declared as a variable), %d kinds of call adapted to it, %d functions still stubbed"
          % ((out / "index.html").relative_to(ROOT), counts["addresses"], counts["divisions"], counts["declarations"] + counts["kinds"],
             counts["kinds"], counts["calls"], len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
