#!/usr/bin/env python3
"""Adapters for calls whose declaration and definition pass structs differently.

The decompilation sometimes declares `f(s32 x, s32 y, void *p)` in one file and defines
`f(struct Position pos, u8 *p)` in another (or the reverse). ARM passes a small struct by value in the same
registers as its words, and 32-bit x86 pushes it as its words, so both forms are the same call there. Every
64-bit convention packs an 8-byte struct into one register, and the two forms stop agreeing.

For 64-bit builds this module finds those declarations, gives each one an adapter that receives the arguments
in the declared form and calls the function in its defined form, and points the declaration at the adapter.
Struct sizes are measured with the compiler. It is called from build.prepare for targets without the 32-bit
Windows symbol prefix; nothing changes for dev32.
"""
import re, subprocess
from pathlib import Path

ARGS = r'\(((?:[^;{}()]|\([^()]*\))*)\)'
DEFINITION = re.compile(r'^(?![ \t#/])([^\n;{}#=]*?)\b(\w+)\s*%s\s*\{' % ARGS, re.M)
DECLARATION = re.compile(r'((?:^|[;{}])\s*(?:extern\s+)?)([A-Za-z_][\w \t\*]*?[ \t\*])(\w+)\s*%s\s*((?:__asm__|asm)\s*\(\s*"(\w+)"\s*\)\s*)?;' % ARGS, re.M)


def split(text):
    text = text.strip()
    if text == "void":
        return []
    if text == "":
        return None
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


TYPEDEFS = set()      # typedef names of struct types in the file being examined (set by apply)


def struct_of(parameter):
    """The struct/union type if the parameter is passed by value (also through a typedef), else None."""
    p = parameter.strip()
    if "*" in p or "[" in p or "(" in p:
        return None
    m = re.match(r'(?:const\s+|volatile\s+)*((?:struct|union)\s+\w+)', p)
    if m:
        return m[1]
    m = re.match(r'(?:const\s+|volatile\s+)*(\w+)', p)
    return m[1] if m and m[1] in TYPEDEFS else None


def typedefs_of(text):
    names = set(re.findall(r'typedef\s+(?:struct|union)\s+\w*\s*\{[^{}]*\}\s*(\w+)\s*;', text))
    return names | set(re.findall(r'typedef\s+(?:struct|union)\s+\w+\s+(\w+)\s*;', text))


def measure(cc, flags, include, path, text, types):
    """sizeof for each struct type, by compiling the file with probes appended."""
    probe = path.with_name(path.stem + "__sizes.c")
    probe.write_text(text + "\n" + "\n".join("const unsigned ss_size_probe_%d = sizeof(%s);" % (i, t) for i, t in enumerate(types)) + "\n",
                     encoding="utf-8")
    done = subprocess.run([cc, *flags, "-iquote", str(include), "-I", str(include), "-S", "-o", "-", str(probe)],
                          capture_output=True, text=True, cwd=include.parent)
    probe.unlink()
    sizes = {}
    for i, t in enumerate(types):
        m = re.search(r'^ss_size_probe_%d:\s*\n\s*\.(?:long|int)\s+(\d+)' % i, done.stdout, re.M)
        if m:
            sizes[t] = int(m[1])
    return sizes


def apply(texts, cc, flags, include):
    """texts: {Path: source}. Returns (new texts, source of the adapters file, report lines)."""
    global TYPEDEFS
    definitions, typedefs = {}, {path: typedefs_of(text) for path, text in texts.items()}
    kinds = lambda path, parameters: (TYPEDEFS.clear(), TYPEDEFS.update(typedefs[path]), [struct_of(p) for p in parameters])[2]
    for path, text in texts.items():
        aliases = {m[3]: m[6] for m in DECLARATION.finditer(text) if m[6]}
        for m in DEFINITION.finditer(text):
            name = aliases.get(m[2], m[2])
            if name not in ("if", "while", "for", "switch", "return", "sizeof"):
                definitions[name] = (split(m[3]), path, "void" in m[1].split() and "*" not in m[1])
    # which declarations disagree in struct-by-value layout
    work, needed, unprototyped = [], {}, []
    for path, text in texts.items():
        for m in DECLARATION.finditer(text):
            real = m[6] or m[3]
            if real not in definitions:
                continue
            declared, (defined, home, _) = split(m[4]), definitions[real]
            if defined is None or "..." in m[4]:
                continue
            if not m[6] and path == home:
                continue                    # a redeclaration next to the definition shares its symbol
            if declared is None:
                unprototyped.append((path, m, real, defined))
                continue
            dk, ek = kinds(path, declared), kinds(home, defined)
            if [bool(k) for k in dk] == [bool(k) for k in ek]:
                continue
            if not any(dk) and not any(ek):
                continue                    # a different count without structs is not this problem
            work.append((path, m, real, declared, defined, dk, ek))
            for k in dk:
                if k: needed.setdefault(path, set()).add(k)
            for k in ek:
                if k: needed.setdefault(home, set()).add(k)
    sizes = {}
    for path, types in needed.items():
        for t, n in measure(cc, flags, include, path, texts[path], sorted(types)).items():
            sizes[(path, t)] = n
    adapters, externs, structs, report, edits, counter = [], set(), set(), [], {}, {}
    def words(type_kinds, where):
        out = []
        for k in type_kinds:
            if k and (where, k) not in sizes:
                return None
            out.append((sizes[(where, k)] + 3) // 4 if k else 1)
        return out
    resolved, forms = [], {}
    for path, m, real, declared, defined, dk, ek in work:
        dw, ew = words(dk, path), words(ek, definitions[real][1])
        if dw is None or ew is None:
            report.append("%s:%s: struct size unknown, left as it is" % (path.name, real))
            continue
        resolved.append((path, m, real, dw, ew))
        forms.setdefault(real, []).append((tuple(dw), tuple(ew)))
    # A declaration without a prototype says nothing about its arguments. Where other files call the same
    # function with structs, assume this one does too and give it the most common of those forms.
    for path, m, real, defined in unprototyped:
        if real in forms:
            dw, ew = max(set(forms[real]), key=forms[real].count)
            resolved.append((path, m, real, list(dw), list(ew)))
            report.append("%s: unprototyped declaration in %s treated as %s words" % (real, path.name, list(dw)))
    for path, m, real, dw, ew in resolved:
        key = (real, tuple(dw))
        if key not in counter:
            counter[key] = "%s__abi%d" % (real, sum(1 for k in counter if k[0] == real))
            returns_void = "void" in m[2].split() and "*" not in m[2]
            def ctype(n):
                if n == 1:
                    return "unsigned int"
                structs.add(n)
                return "struct ss_words%d" % n
            params = ", ".join("%s p%d" % (ctype(n), i) for i, n in enumerate(dw)) or "void"
            body = ["%s %s(%s)" % ("void" if returns_void else "unsigned int", counter[key], params), "{",
                    "    unsigned int w[%d] = { 0 };" % max(sum(dw), sum(ew), 1)]
            at = 0
            for i, n in enumerate(dw):
                body.append("    w[%d] = p%d;" % (at, i) if n == 1 else "    __builtin_memcpy(w + %d, &p%d, %d);" % (at, i, n * 4))
                at += n
            call_types, call_args, at = [], [], 0
            for i, n in enumerate(ew):
                call_types.append(ctype(n))
                if n == 1:
                    call_args.append("w[%d]" % at)
                else:
                    body.append("    struct ss_words%d a%d; __builtin_memcpy(&a%d, w + %d, %d);" % (n, i, i, at, n * 4))
                    call_args.append("a%d" % i)
                at += n
            cast = "(%s (*)(%s))" % ("void" if returns_void else "unsigned int", ", ".join(call_types) or "void")
            body.append("    %s(%s%s)(%s);" % ("" if returns_void else "return ", cast, real, ", ".join(call_args)))
            body.append("}")
            adapters.append("\n".join(body))
            externs.add(real)
            report.append("%s: declared as %s words in %s, defined as %s words" % (real, dw, path.name, ew))
        edits.setdefault(path, []).append((m, counter[key]))
    new_texts = dict(texts)
    for path, items in edits.items():
        text = texts[path]
        for m, thunk in sorted(items, key=lambda item: -item[0].start()):
            if m[6]:
                a, b = m.start(6), m.end(6)
                text = text[:a] + thunk + text[b:]
            else:
                text = text[:m.end(4) + 1] + ' asm("%s")' % thunk + text[m.end(4) + 1:]
        new_texts[path] = text
    source = ["/* Generated by tools/abi_thunks.py: adapters between declared and defined argument layouts. */"]
    source += ["struct ss_words%d { unsigned int w[%d]; };" % (n, n) for n in sorted(structs)]
    source += ["void %s();" % n for n in sorted(externs)]
    source += adapters
    return new_texts, "\n".join(source) + "\n", report
