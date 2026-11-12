#!/usr/bin/env python3
"""check_calls.py [--game ss2] : find calls that pass fewer arguments than the function takes.

On ARM a matching decompilation can call `g()` right after `x = f()` and rely on the result of `f` still being
in register r0, which is where `g` expects its first argument. That is invisible in the ROM and wrong everywhere
else. This tool compares every call with the parameter count of the definition (through asm("name") aliases
too) and lists the calls that pass fewer. Run `build.py prepare` first.
"""
import argparse, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build

DEFINITION = re.compile(r'^(?![ \t#/])[^\n;{}#=]*?\b(\w+)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*\{', re.M)
ALIAS = re.compile(r'\b(\w+)\s*\([^;{}]*\)\s*(?:__asm__|asm)\s*\(\s*"_?(\w+)"\s*\)\s*;')


def count_parameters(text):
    text = text.strip()
    if text in ("", "void"):
        return 0
    if "..." in text:
        return None
    depth, count = 0, 1
    for ch in text:
        depth += ch in "(["
        depth -= ch in ")]"
        count += ch == "," and depth == 0
    return count


def count_arguments(text, start):
    """Arguments of the call whose '(' is at `start`; None if the parenthesis does not close."""
    depth, count, empty, i = 0, 1, True, start
    while i < len(text):
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                return 0 if empty else count
        elif depth == 1:
            if ch == ",":
                count += 1
            elif not ch.isspace():
                empty = False
        i += 1
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    args = parser.parse_args()
    gen = build.ROOT / "build/gen" / args.game / "src"
    replaced = build.port_config(args.game)["runtime"]
    files = {p: p.read_text(encoding="utf-8", errors="replace") for p in sorted(gen.glob("*.c")) if p.stem not in replaced}
    parameters = {}
    for text in files.values():
        aliases = {local: real for local, real in ALIAS.findall(text)}
        for m in DEFINITION.finditer(text):
            name = aliases.get(m[1], m[1])
            if name not in ("if", "while", "for", "switch", "return", "sizeof"):
                parameters[name] = count_parameters(m[2])
    found = 0
    for path, text in files.items():
        aliases = {local: real for local, real in ALIAS.findall(text)}
        for m in re.finditer(r'\b(\w+)\s*\(', text):
            name = aliases.get(m[1], m[1])
            wanted = parameters.get(name)
            if not wanted:
                continue
            line_start = text.rfind("\n", 0, m.start()) + 1
            before = text[line_start:m.start()]
            if DEFINITION.match(text, line_start) and text[line_start:m.end()].count("(") == 1 and not before.strip().endswith(("=", ",", "(", "return")):
                if re.match(r'^(?![ \t])', text[line_start:]):      # the definition or a file-scope declaration
                    continue
            if re.search(r'\b(?:extern|void|u8|u16|u32|s8|s16|s32|int|struct\s+\w+|static)\s*\**\s*$', before) or "asm(" in text[m.end():text.find("\n", m.end())]:
                continue                                           # a declaration, not a call
            given = count_arguments(text, m.end() - 1)
            if given is not None and given < wanted:
                found += 1
                line = text.count("\n", 0, m.start()) + 1
                print("%s:%d: %s takes %d argument(s), called with %d:  %s" % (
                    path.name, line, name, wanted, given, text[line_start:text.find("\n", m.start())].strip()[:110]))
    print("%d call(s) pass fewer arguments than the function takes" % found)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
