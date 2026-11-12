#!/usr/bin/env python3
"""Make every function narrow its own small arguments, as the console's compiler did.

The decompilation often declares the same function with u16 in one file and s32 (or no prototype at all) in
another. On the console and on x86 that is harmless: each argument travels in a full word and the function
itself cuts it down to its type. The arm64_32 calling convention differs twice: the caller is trusted to have
extended a small argument (the function does not cut it down again), and small arguments on the stack are
packed (a u16 takes two bytes), so a caller with another declaration puts them in other places.

For that target this module rewrites the generated sources: in every definition a parameter narrower than 32
bits becomes 32 bits wide and is narrowed in a local of the original type, so the body is unchanged; every
declaration gets the same 32-bit parameters, so callers pass full words. Called from build.prepare for the
Mach-O image only.
"""
import re

import abi_thunks

NARROW = {
    "u8": "u32", "u16": "u32", "s8": "s32", "s16": "s32", "bool8": "u32", "bool16": "u32", "char": "s32", "short": "s32",
    "unsigned char": "u32", "unsigned short": "u32", "signed char": "s32", "signed short": "s32", "short int": "s32",
    "unsigned short int": "u32", "vu8": "u32", "vu16": "u32", "vs8": "s32", "vs16": "s32",
}
PARAMETER = re.compile(r'^\s*((?:const\s+|volatile\s+|register\s+)*)([A-Za-z_][\w ]*?)\s*(\b[A-Za-z_]\w*)?\s*$')


def narrow_type(parameter):
    """(qualifiers, type, name) if the parameter is a plain integer narrower than 32 bits, else None."""
    if "*" in parameter or "[" in parameter or "(" in parameter:
        return None
    m = PARAMETER.match(parameter)
    if not m:
        return None
    kind, name = m[2].strip(), m[3]
    if kind in NARROW:
        return m[1], kind, name
    if name and (kind + " " + name) in NARROW:          # "unsigned short" with no parameter name
        return m[1], kind + " " + name, None
    return None


def widen(text, report, where):
    edits = []
    for m in abi_thunks.DEFINITION.finditer(text):
        parameters = abi_thunks.split(m[3])
        if not parameters:
            continue
        locals_, changed = [], list(parameters)
        for i in range(len(parameters)):
            found = narrow_type(parameters[i])
            if found and found[2]:
                qualifiers, kind, name = found
                changed[i] = " %s %s__wide" % (NARROW[kind], name)
                locals_.append(" %s %s = (%s)%s__wide;" % (kind, name, kind, name))
        if locals_:
            edits.append((m.start(3), m.end(3), ",".join(changed)))
            edits.append((m.end(), m.end(), "".join(locals_)))
            report.append("%s: %s narrows %d argument(s) itself" % (where, m[2], len(locals_)))
    for m in abi_thunks.DECLARATION.finditer(text):
        parameters = abi_thunks.split(m[4])
        if not parameters:
            continue
        changed, touched = list(parameters), False
        for i in range(len(parameters)):
            found = narrow_type(parameters[i])
            if found:
                qualifiers, kind, name = found
                changed[i] = " %s%s" % (NARROW[kind], " " + name if name else "")
                touched = True
        if touched:
            edits.append((m.start(4), m.end(4), ",".join(changed)))
    for start, end, new in sorted(edits, key=lambda e: (-e[0], -e[1])):
        text = text[:start] + new + text[end:]
    return text


def apply(pending):
    """pending: {path: text}. Returns the list of what was changed; texts are updated in place."""
    report = []
    for target in list(pending):
        if target.suffix in (".c", ".h", ".inc"):
            pending[target] = widen(pending[target], report, target.name)
    return report
