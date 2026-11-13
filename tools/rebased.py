#!/usr/bin/env python3
"""What the builds with rebased GBA memory share (tools/rebase_ir.py): the PC check build, the Vita, the Switch.

Every module that touches game memory goes through LLVM IR before any optimisation:
  game modules   fixed addresses become constants (absolute_ir), every division gets the cartridge's results
                 (division_ir, "all": no processor's own behaviour is relied on), then rebase_ir with the test
                 for addresses the console would have ignored;
  runtime core   absolute_ir and rebase_ir without that test (the core makes no such access).
Other sources (start-up, platform) are compiled as they are and reach game memory through ss_host().
"""
import concurrent.futures, json, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import absolute_ir, build, division_ir, link, rebase_ir

ROOT = build.ROOT


def resolver(game):
    named = link.named_addresses(game)

    def resolve(name):
        name = name[1:] if name.startswith("_") else name
        m = link.ADDRESS.match(name)
        if m:
            return int(m[1], 16) + int(m[2] or 0)
        return named.get(name)
    return resolve


def compile_units(cc, units, out, codegen, stray, resolve, jobs, prefix):
    """units: (source, flags, cwd, kind) with kind "game", "core" or "plain"; codegen: target and processor flags;
    prefix: the target puts an underscore before C names.
    Returns the objects, in order, and prints the totals of the rewriting."""
    for sub in ("ir", "obj"):
        (out / sub).mkdir(parents=True, exist_ok=True)
    totals = {}

    def plain_name(name):
        name = name.strip('"').replace("\\01", "")
        if prefix and name.startswith("_"):
            name = name[1:]
        return re.sub(r'^(?:__wrap_|__real_)', "", name)

    def to_ir(unit):
        path, flags, cwd, kind = unit
        target = out / "obj" / (path.stem + ".o")
        if kind == "plain":
            done = subprocess.run([cc, *codegen, *flags, "-c", str(path), "-o", str(target)], capture_output=True, text=True, cwd=cwd)
            return path.name, done.returncode, done.stderr
        ir = out / "ir" / (path.stem + ".ll")
        done = subprocess.run([cc, *codegen, *flags, "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm", str(path), "-o", str(ir)],
                              capture_output=True, text=True, cwd=cwd)
        return path.name, done.returncode, done.stderr
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        failed = [(name, errors) for name, code, errors in pool.map(to_ir, units) if code]
    if failed:
        for name, errors in failed[:3]:
            print(name, "\n", "\n".join(l for l in errors.splitlines() if "error" in l)[:2500])
        sys.exit("%d file(s) do not compile" % len(failed))

    # rule 5 of rebase_ir: the game's functions, and the runtime's functions the game names
    texts = {u[0]: (out / "ir" / (u[0].stem + ".ll")).read_text(encoding="utf-8") for u in units if u[3] != "plain"}
    game, core, named = {}, {}, set()
    for unit in units:
        if unit[3] == "plain":
            continue
        found = rebase_ir.signatures(texts[unit[0]], plain_name)
        (game if unit[3] == "game" else core).update(found)
        if unit[3] == "game":
            named.update(plain_name(m) for m in re.findall(r'^declare [^@\n]*@("[^"]+"|[-\w.$]+)\(', texts[unit[0]], re.M))
    game.update({name: kinds for name, kinds in core.items() if name in named or name in game})

    def one(unit):
        path, flags, cwd, kind = unit
        target = out / "obj" / (path.stem + ".o")
        if kind == "plain":
            return path.name, 0, "", {}
        ir = out / "ir" / (path.stem + ".ll")
        text, _ = absolute_ir.lower(texts[path], resolve)
        if kind == "game":
            text, _ = division_ir.lower(text, "all")
        text, counts = rebase_ir.lower(text, kind == "game", stray, game, plain_name)
        ir.write_text(text, encoding="utf-8")
        level = "-O1" if kind == "game" else "-O2"
        done = subprocess.run([cc, *codegen, level, "-w", "-c", str(ir), "-o", str(target)], capture_output=True, text=True)
        return path.name, done.returncode, done.stderr, counts
    failed = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, code, errors, counts in pool.map(one, units):
            if code:
                failed.append((name, errors))
            for key, value in counts.items():
                totals[key] = totals.get(key, 0) + value
    if failed:
        for name, errors in failed[:3]:
            print(name, "\n", "\n".join(l for l in errors.splitlines() if "error" in l)[:2500])
        sys.exit("%d file(s) do not compile" % len(failed))
    print("rebased: " + ", ".join("%d %s" % (v, k) for k, v in totals.items()))
    return [out / "obj" / (u[0].stem + ".o") for u in units]


def tables(game, out, objects, prefix, wraps_renamed=False, keep=()):
    """stubs.c and codetable.c for what the objects still need, as tools/link.py writes them. A name with a fixed
    address must not be left: it would be a hardware address nobody rebased. prefix: the target's C prefix.
    keep: undefined names another part of the link provides."""
    strip = (lambda n: n[1:] if n.startswith("_") else n) if prefix else (lambda n: n)
    listing = lambda flags: link.nm([*flags, *map(str, objects)]).splitlines()
    defined = {strip(l.split()[-1]) for l in listing(["--defined-only"]) if len(l.split()) == 3}
    undefined = {strip(l.split()[-1]) for l in listing(["--undefined-only"]) if len(l.split()) in (1, 2) and not l.rstrip().endswith(":")}
    named, functions = link.named_addresses(game), link.function_names(game)
    left = sorted(n for n in undefined - defined if link.ADDRESS.match(n) or n in named)
    if left:
        sys.exit("names with a fixed address were not rebased:\n  " + "\n  ".join(left[:20]))
    host = {"memcpy", "memset", "memmove", "abs"}          # the C library's, which the game also names
    stubs = sorted(n for n in undefined - defined - set(keep) - set(link.WRAPS) - host
                   if (n in functions or re.fullmatch(r"sub_08[0-9A-F]{6}", n)) and not n.startswith("__"))
    (out / "stubs.c").write_text('#include "shiningsource/runtime.h"\n' + "\n".join(
        "int %s(void) { static int seen; ss_stub(\"%s\", &seen); return 0; }" % (n, n) for n in stubs) + "\n", encoding="utf-8")
    entries = []
    for line in (build.GAMES[game] / "tools/config/functions.cfg").read_text(encoding="utf-8").splitlines():
        fields = line.split("#")[0].split()
        if len(fields) >= 2:
            address = int(fields[1], 16) & ~1
            name = fields[2] if len(fields) > 2 else "sub_%08X" % address
            if wraps_renamed and name in link.WRAPS:
                name = "__wrap_" + name
            if name in defined or name in stubs:
                entries.append((address | (fields[0] == "thumb"), name))
    entries.sort()
    ranges = json.loads((ROOT / "games" / game / "port/code_pointer_ranges.json").read_text(encoding="utf-8"))["ranges"]
    table = ["#include <stdint.h>", "struct ss_code_entry { uint32_t gba; void (*native)(void); };"]
    table += ["void %s();" % n for n in sorted({n for _, n in entries})]
    table += ["const struct ss_code_entry ss_code_table[] = {"] + ["    { 0x%08Xu, (void (*)(void))%s }," % e for e in entries] + ["};"]
    table += ["const unsigned ss_code_count = %d;" % len(entries),
              "const uint32_t ss_code_ranges[][2] = { %s };" % ", ".join("{ %s, %s }" % (a, b) for a, b in ranges),
              "const unsigned ss_code_range_count = %d;" % len(ranges)]
    (out / "codetable.c").write_text("\n".join(table) + "\n", encoding="utf-8")
    return stubs
