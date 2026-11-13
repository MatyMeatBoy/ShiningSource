#!/usr/bin/env python3
"""Turn the game's variables at fixed addresses into constants, for targets whose linker cannot place a symbol.

The decompilation names memory by address (`gUnk_03001234`, the registers, labels inside the ROM). The native
builds give those names their addresses at link time. WebAssembly's object format has no absolute symbols, so
for that target the address is written into the code instead: in the LLVM IR of each module the declaration of
such a variable is removed and every use of it becomes `inttoptr (i32 ADDRESS to ptr)`.

  lower(text, resolve)    resolve(name) returns the address of a name or None; returns (new text, count)
"""
import re

# LLVM writes a name without quotes when it only has letters, digits and - $ . _ ("gUnk_03005DDC-28" included)
DECLARATION = re.compile(r'^@(?:"((?:[^"\\]|\\.)+)"|([-\w.$]+)) = external [^\n]*$', re.M)


def lower(text, resolve):
    replaced = {}
    for m in DECLARATION.finditer(text):
        raw = m[1] if m[1] is not None else m[2]
        name = raw[3:] if raw.startswith("\\01") else raw
        address = resolve(name)
        if address is not None:
            replaced[raw] = address
    if not replaced:
        return text, 0
    lines = []
    for line in text.split("\n"):
        m = DECLARATION.match(line)
        if m and (m[1] if m[1] is not None else m[2]) in replaced:
            continue
        lines.append(line)
    text = "\n".join(lines)
    for raw, address in sorted(replaced.items(), key=lambda item: -len(item[0])):
        constant = "inttoptr (i32 %d to ptr)" % (address if address < 0x80000000 else address - 0x100000000)
        if re.fullmatch(r'[-\w.$]+', raw):
            text = re.sub(r'@%s(?![-\w.$])' % re.escape(raw), constant, text)
        text = text.replace('@"%s"' % raw, constant)
    return text, len(replaced)
