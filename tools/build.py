#!/usr/bin/env python3
"""Build driver for ShiningSource.

  build.py prepare [--game ss2]          generate port sources from the decomp into build/gen/<game>
  build.py compile [--game ss2] [-j N]   prepare, then compile every game module for the dev32 target
  build.py fix     [--game ss2]          compile, learn which prototypes must be hoisted, repeat until stable
  build.py report  [--game ss2]          print the last compile report

The decomp is never edited from here. `prepare` copies its C and headers and applies the mechanical
changes a non-ARM compiler needs (PREPARE_STEPS, plus the per-game files in games/<game>/port/).
`compile` is the first gate of the port: every game module must become an object file for a
32-bit-pointer target. The report groups what is left by error class, so progress is a number.

Targets (docs/ARCHITECTURE.md): dev32 is the development build, a 32-bit Windows process in which GBA
addresses are real addresses. The shipping builds are 64-bit hosts running the same game code compiled
with 32-bit pointers; they reuse everything dev32 proves.
"""
import argparse, concurrent.futures, json, os, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAMES = {"ss2": ROOT / "games/ss2/decomp"}
TOOLCHAIN_HINTS = [os.environ.get("SHININGSOURCE_LLVM_MINGW", ""), "D:/01Claude/toolchain/llvm-mingw-20261006-ucrt-x86_64",
                   "C:/llvm-mingw"]

# `register T x asm("r4")` (or asm(MACRO_ARGUMENT)) only steers the original ARM compiler.
ARM_PIN = re.compile(r'\bregister\s+([^;=(){}]*?)\s+asm\s*\(\s*(?:"(?:r\d+|sl|sb|fp|ip|lr|sp)"|[A-Za-z_]\w*)\s*\)')
ARM_PIN_FUNCPTR = re.compile(r'\bregister\s+([\w\s\*]*\(\s*\*\s*\w+\s*\)\s*\([^()]*\))\s+asm\s*\(\s*"(?:r\d+|sl|sb|fp|ip|lr|sp)"\s*\)')
# asm("") with operands or clobbers emits no instruction: it is an allocation barrier for agbcc only.
EMPTY_ASM = re.compile(r'\basm(?:\s+volatile)?\s*\(\s*""\s*(?::[^;]*?)?\)\s*;')
# `asm volatile("movs %0, #1" : "=r"(x))` loads a constant with a chosen instruction; in C it is an assignment.
MOVS_CONST = re.compile(r'asm\s+volatile\s*\(\s*"movs %0, #(\d+)"\s*:\s*"=r"\s*\((\w+)\)\s*\)\s*;')
LTORG = re.compile(r'asm\s+volatile\s*\(\s*"\.ltorg"\s*\)\s*;')
# `extern T name asm("gUnk_03001234")` gives a C object the assembler name of the ROM symbol. 32-bit Windows
# prefixes C symbols with an underscore, so explicit assembler names get the same prefix to keep one namespace.
ASM_LABEL = re.compile(r'(\b(?:__asm__|asm)\s*\(\s*")([A-Za-z_]\w*(?:[-+]\d+)?)("\s*\))')
# the same through a stringizing macro: asm(#name)
ASM_LABEL_MACRO = re.compile(r'(\b(?:__asm__|asm)\s*\(\s*)#\s*(\w+)(\s*\))')
PREPARE_STEPS = [
    ("constant loads written as ARM instructions", lambda text: MOVS_CONST.sub(lambda m: "%s = %s;" % (m[2], m[1]), text)),
    ("literal pool placement", lambda text: LTORG.sub(";", text)),
    ("strip ARM register pins on function pointers", lambda text: ARM_PIN_FUNCPTR.sub(lambda m: m[1], text)),
    ("drop empty asm barriers", lambda text: EMPTY_ASM.sub(";", text)),
    ("strip ARM register pins", lambda text: ARM_PIN.sub(lambda m: m[1], text)),
    ("assembler names follow the platform's C prefix", lambda text: ASM_LABEL.sub(lambda m: m[1] + "_" + m[2] + m[3], text)),
    ("assembler names through a stringizing macro", lambda text: ASM_LABEL_MACRO.sub(lambda m: m[1] + '"_" #' + m[2] + m[3], text)),
    # global.h pulls the ARM assembler macros into every translation unit
    ("drop file-scope ARM assembler include", lambda text: text.replace('asm(".include \\"asm/macros.inc\\"");', "")),
]
CFLAGS = ["--target=i686-w64-mingw32", "-std=gnu89", "-O1", "-g", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char",
          "-fno-builtin", "-ferror-limit=0", "-w",
          # The cartridge reads a few locals before writing them; on the console the value is whatever the
          # register held. Zero on every platform keeps all builds identical, which online play requires.
          "-ftrivial-auto-var-init=zero",
          # agbcc accepts these; they are findings to clean in the decomp over time, not blockers
          "-Wno-error=int-conversion", "-Wno-error=return-mismatch", "-Wno-error=incompatible-pointer-types",
          "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int", "-Wno-error=return-type",
          "-DSHININGSOURCE=1", "-DPORT=1"]


def clang():
    for hint in TOOLCHAIN_HINTS:
        exe = Path(hint) / "bin/clang.exe"
        if hint and exe.exists():
            return str(exe)
    found = shutil.which("clang")
    if found:
        return found
    sys.exit("clang not found: set SHININGSOURCE_LLVM_MINGW to an llvm-mingw directory")


def port_config(game):
    """games/<game>/port/modules.json: {"runtime": {module: reason}} lists decomp modules the runtime replaces."""
    path = ROOT / "games" / game / "port/modules.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {"runtime": {}}


def hoists(game):
    path = ROOT / "games" / game / "port/hoist.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def prototype(text, name):
    """The declaration of `name` at file scope: an extern line, or the header of its definition."""
    args = r'\((?:[^;{}()]|\([^()]*\))*\)'
    # 1. the definition: it has the real parameter types (a K&R `extern void f();` does not)
    m = re.search(r'^(?![ \t#/])([^\n;{}#]*\b%s\s*%s)\s*\{' % (re.escape(name), args), text, re.M)
    if m:
        return m[1].strip() + ";"
    # 2. a declaration with parameters, at file scope or inside a function body
    kind = r'(?:(?:const|unsigned|signed|struct|union)\s+)*[A-Za-z_]\w*[ \t\*]+'      # a return type must be present
    for m in re.finditer(r'^[ \t]*((?:extern\s+)?%s%s\s*%s)\s*(?:asm\s*\([^)]*\)\s*)?;' % (kind, re.escape(name), args), text, re.M):
        if not re.search(r'\(\s*\)$', m[1].strip()):
            return m[1].strip() + ";"
    return None


def forward_structs(protos):
    """`struct X;` lines so a hoisted prototype and the later definition name the same type."""
    tags = sorted({"%s %s;" % (kind, tag) for p in protos for kind, tag in re.findall(r'\b(struct|union)\s+(\w+)', p)})
    return "\n".join(tags) + ("\n" if tags else "")


def prepare(game, prefix=True, abi_flags=None):
    """prefix: the target prepends an underscore to C symbols (32-bit Windows); ELF targets do not."""
    src, out = GAMES[game], ROOT / ("build/gen" if prefix else "build/gen-elf") / game
    if not (src / "src").is_dir():
        sys.exit("decomp submodule missing: git submodule update --init")
    hoist = hoists(game)
    patch_file = ROOT / "games" / game / "port/patches.json"
    patches = json.loads(patch_file.read_text(encoding="utf-8")) if patch_file.exists() else {}
    pending = {}
    for sub in ("src", "include"):
        for path in sorted((src / sub).rglob("*")):
            if path.suffix not in (".c", ".h", ".inc"):
                continue
            text = path.read_bytes().decode("utf-8", errors="replace")
            for label, step in PREPARE_STEPS:
                if prefix or not label.startswith("assembler names"):
                    text = step(text)
            for patch in patches.get(path.name, []):
                if text.count(patch["find"]) != 1:
                    sys.exit("%s: patch no longer applies exactly once: %s" % (path.name, patch["why"]))
                text = text.replace(patch["find"], patch["replace"])
            names = hoist.get(path.stem, []) if sub == "src" else []
            found = [(n, prototype(text, n)) for n in names]
            protos = [p for _, p in found if p]
            if protos:
                for n, p in found:
                    if p:       # a declaration without parameters cannot stand next to the real prototype
                        text = re.sub(r'^[ \t]*(?:extern\s+)?[\w \t\*]+\b%s\s*\(\s*\)\s*;[ \t]*\n' % re.escape(n), "", text, flags=re.M)
                        # nor can a block-scope redeclaration with other parameter types
                        text = re.sub(r'^[ \t]+(?:extern\s+)?[A-Za-z_][\w \t\*]*[ \t\*]%s\s*\((?:[^;{}()]|\([^()]*\))*\)\s*;[ \t]*\n'
                                      % re.escape(n), "", text, flags=re.M)
                # after the includes, before first use: the function is called before the file declares it
                at = max((m.end() for m in re.finditer(r'^#include[^\n]*\n', text, re.M)), default=0)
                text = (text[:at] + "/* hoisted by tools/build.py: used before declared */\n" + forward_structs(protos)
                        + "\n".join(protos) + "\n" + text[at:])
            pending[out / path.relative_to(src)] = text
    if abi_flags is not None:
        # 64-bit conventions: adapters where a declaration and the definition pass structs differently
        import abi_thunks
        replaced = port_config(game)["runtime"]
        for target, text in pending.items():        # headers must exist for the size probes
            if target.suffix != ".c":
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(text.encode("utf-8"))
        modules = {t: s for t, s in pending.items() if t.suffix == ".c" and t.parent.name == "src" and t.stem not in replaced}
        (out / "src").mkdir(parents=True, exist_ok=True)
        fixed, adapters, report = abi_thunks.apply(modules, clang(), abi_flags, out / "include")
        pending.update(fixed)
        pending[out / "src/abi_thunks.c"] = adapters
        (out / "abi_thunks.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    for target, text in pending.items():
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes().decode("utf-8", errors="replace") != text:
            target.write_bytes(text.encode("utf-8"))
    return out


def classify(message):
    message = re.sub(r"'[^']*'", "X", message)
    return re.sub(r"\s*\[-W[^\]]*\]$", "", message)


def compile_all(game, jobs):
    gen = prepare(game)
    objdir = ROOT / "build/dev32" / game / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    cc = clang()
    replaced = port_config(game)["runtime"]
    sources = [p for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced]

    def one(path):
        cmd = [cc, *CFLAGS, "-iquote", str(gen / "include"), "-I", str(gen / "include"), "-I", str(ROOT / "runtime/include"),
               "-c", str(path), "-o", str(objdir / (path.stem + ".o"))]
        run = subprocess.run(cmd, capture_output=True, text=True, cwd=gen)
        return path.stem, run.returncode, re.findall(r"^(.*?):(\d+):\d+: error: (.*)$", run.stderr, re.M)

    report = {"game": game, "target": "dev32", "modules": len(sources), "compiled": 0, "runtime_modules": len(replaced),
              "errors": 0, "classes": {}, "failed": {}, "conflicts": {}}
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, code, errors in pool.map(one, sources):
            if code == 0:
                report["compiled"] += 1
                continue
            report["failed"][name] = len(errors)
            report["errors"] += len(errors)
            for _, _, message in errors:
                key = classify(message)
                report["classes"][key] = report["classes"].get(key, 0) + 1
                m = re.match(r"conflicting types for '(\w+)'", message)
                if m:
                    report["conflicts"].setdefault(name, []).append(m[1])
    (ROOT / "build/dev32" / game / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    return report


def fix(game, jobs):
    path = ROOT / "games" / game / "port/hoist.json"
    for _ in range(6):
        report = compile_all(game, jobs)
        if not report["conflicts"]:
            break
        table = hoists(game)
        for module, names in report["conflicts"].items():
            table[module] = sorted(set(table.get(module, [])) | set(names))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(dict(sorted(table.items())), indent=1) + "\n", encoding="utf-8", newline="\n")
    return report


def show(report):
    print("%s / %s: %d of %d game modules compile, %d error(s); %d module(s) replaced by the runtime" % (
        report["game"], report["target"], report["compiled"], report["modules"], report["errors"], report["runtime_modules"]))
    for key, count in sorted(report["classes"].items(), key=lambda kv: -kv[1]):
        print("  %4d  %s" % (count, key))
    return 0 if report["compiled"] == report["modules"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["prepare", "compile", "fix", "report"])
    parser.add_argument("--game", default="ss2", choices=sorted(GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    if args.command == "prepare":
        print(prepare(args.game))
        return 0
    if args.command == "compile":
        return show(compile_all(args.game, args.j))
    if args.command == "fix":
        return show(fix(args.game, args.j))
    path = ROOT / "build/dev32" / args.game / "report.json"
    if not path.exists():
        sys.exit("no report yet: run build.py compile")
    return show(json.loads(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    sys.exit(main())
