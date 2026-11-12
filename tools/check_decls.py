#!/usr/bin/env python3
"""check_decls.py [--game ss2] : declarations whose parameters disagree with the function's definition in a way
that only works on ARM.

A matching decompilation may declare a callee differently in each file to steer the original compiler. Two
shapes produce the right ARM code and the wrong code everywhere else:

  struct by value   declared `f(struct S)`, defined `f(struct S *)` (or the reverse): ARM passes the first words
                    of the struct in the registers where the pointer is expected
  different count   declared with fewer or more parameters than the definition

Each finding names the declaration to fix **in the decompilation**; the ROM must still match afterwards.
Run `build.py prepare` first.
"""
import argparse, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build

ARGS = r'\(((?:[^;{}()]|\([^()]*\))*)\)'
DEFINITION = re.compile(r'^(?![ \t#/])[^\n;{}#=]*?\b(\w+)\s*%s\s*\{' % ARGS, re.M)
DECLARATION = re.compile(r'(?:^|[;{}])\s*(?:extern\s+)?[A-Za-z_][\w \t\*]*?[ \t\*](\w+)\s*%s\s*(?:(?:__asm__|asm)\s*\(\s*"_?(\w+)"\s*\)\s*)?;' % ARGS, re.M)


def split(text):
    text = text.strip()
    if text in ("", "void"):
        return [] if text == "void" else None       # None: unspecified (K&R)
    parts, depth, current = [], 0, ""
    for ch in text:
        depth += ch in "(["
        depth -= ch in ")]"
        if ch == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += ch
    return parts + [current]


def kind(parameter):
    """'value' for a struct or union passed by value, 'other' for everything else."""
    p = parameter.strip()
    if re.match(r'(?:const\s+)?(?:struct|union)\s+\w+', p) and "*" not in p and "[" not in p and "(" not in p:
        return "value"
    return "other"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    args = parser.parse_args()
    gen = build.ROOT / "build/gen" / args.game / "src"
    replaced = build.port_config(args.game)["runtime"]
    files = {p: p.read_text(encoding="utf-8", errors="replace") for p in sorted(gen.glob("*.c")) if p.stem not in replaced}
    definitions = {}
    for path, text in files.items():
        aliases = {m[1]: m[3] for m in DECLARATION.finditer(text) if m[3]}
        for m in DEFINITION.finditer(text):
            name = aliases.get(m[1], m[1])
            if name not in ("if", "while", "for", "switch", "return", "sizeof"):
                definitions[name] = (split(m[2]), path.name)
    found = 0
    for path, text in files.items():
        for m in DECLARATION.finditer(text):
            name = m[3] or m[1]
            if name not in definitions:
                continue
            declared, (defined, where) = split(m[2]), definitions[name]
            if declared is None or defined is None or "..." in m[2]:
                continue
            line = text.count("\n", 0, m.start(1)) + 1
            problem = None
            if any(kind(p) == "value" for p in declared) != any(kind(p) == "value" for p in defined):
                problem = "struct by value on one side only"
            elif len(declared) != len(defined) and not any(kind(p) == "value" for p in declared + defined):
                problem = "%d parameter(s) declared, %d defined" % (len(declared), len(defined))
            if problem:
                found += 1
                print("%s:%d: %s (%s): %s\n      declared (%s)\n      defined  (%s) in %s" % (
                    path.name, line, m[1], name, problem, " ".join(m[2].split())[:120],
                    ", ".join(" ".join(p.split()) for p in defined)[:120], where))
    print("%d declaration(s) disagree with their definition" % found)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
