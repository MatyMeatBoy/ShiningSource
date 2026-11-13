#!/usr/bin/env python3
"""Move the GBA's memory away from its hardware addresses, for consoles that cannot map them.

The other builds give the game its memory at the console's real addresses. The Switch (no address below
0x08000000 for a homebrew program) and the Vita (user memory starts at 0x81000000) cannot. There the regions and
the ROM lie one after another in one block of the program, `ss_arena` (runtime/console/rebase.c), and this
module rewrites the LLVM IR of every module that touches game memory, before any optimisation:

  The game's modules (guard=True). The decompiled C treats addresses as numbers freely (a pointer made from an
  offset and then moved by an address, a parameter declared as a number in one file and as a pointer in
  another), so nothing is assumed about its values: every pointer the game holds, in memory or in a register,
  is what the cartridge would hold, and the move into the block happens where memory is touched:
    - a load or store whose address is not plainly a local or a global of the program finds its place with
      `ss.g2h` (one table look-up by the top byte and an addition; an address of no GBA region, the program's
      own data and stack among them, is left as it is);
    - an address that is a constant (the game's variables at fixed addresses after tools/absolute_ir.py, the
      registers) becomes `ss_arena + offset`: the linker's work, nothing at run time;
    - the cartridge reads and writes through null and through one bad pointer; the console ignores it and the
      other builds absorb the fault. Here the same test point asks `ss.stray`: such an address reads 0 and
      writes into a scratch block. No fault handler is needed;
    - pointers given to a function that is not the game's (memcpy and the like) are moved first.

  The runtime core (guard=False) is written for this port and keeps its types straight, and its inner loops
  (the renderer, the sound mixer) should not pay per access. There a pointer in a register is where the program
  really finds the thing, and a pointer in memory is the cartridge's value: a pointer read from memory or made
  from a number goes through `ss.g2h`, one written to memory or turned into a number through `ss.h2g`; a local
  pointer variable whose address is never taken stays as it is. Both steps leave what is already converted
  alone, so they may meet twice. Where the core and the game call each other (the functions in `game`, and
  every call through a pointer) pointer arguments and results travel as the cartridge's values.

  signatures(text, norm)                   {function: [parameter kinds]} of the functions a module defines
  lower(text, guard, stray, game, norm)    returns (new text, counts). stray: IR text of the test, see STRAY_BELOW;
                                           game: the functions of the game and those of the core the game names;
                                           norm: a function's name as the IR writes it -> its plain name.
"""
import re

SLOT = 0x80000                  # a region's share of the block; the largest region (EWRAM) is half of that
REGIONS = range(2, 8)           # top bytes 02..07; 08 is the ROM, 16 MiB after them
ROM_AT, ROM_SIZE = len(REGIONS) * SLOT, 0x1000000
ARENA = ROM_AT + ROM_SIZE + 0x10000
BACK = ARENA // SLOT + 1        # entries of ss_back; the last is the "not in the block" entry (0)

NAME = r'%(?:"[^"]+"|[-\w.$]+)'
CONST_ADDRESS = re.compile(r'inttoptr \(i32 (-?\d+) to ptr\)')
FOLD = re.compile(r'ptrtoint \(ptr inttoptr \(i32 (-?\d+) to ptr\) to i32\)')
ALLOCA = re.compile(r'^\s*(%s) = alloca ([^,]+)' % NAME)
GEP = re.compile(r'^\s*(%s) = getelementptr (?:inbounds |nuw |nusw )*(.*)$' % NAME)
LOAD = re.compile(r'^(\s*)(%s) = load (volatile )?(.*)$' % NAME)
STORE = re.compile(r'^(\s*)store (volatile )?(.*)$')
INTTOPTR = re.compile(r'^(\s*)(%s) = inttoptr (i\d+) (.+?) to ptr(.*)$' % NAME)
PTRTOINT = re.compile(r'^(\s*)(%s) = ptrtoint ptr (.+?) to (i\d+)(.*)$' % NAME)
SCALAR = {"i8", "i16", "i32", "i64", "ptr", "float", "double"}
DEFINE = re.compile(r'^define [^@\n]*@("[^"]+"|[-\w.$]+)\((.*)\)[^()]*\{\s*$')
CALL = re.compile(r'^(\s*)(?:(%s) = )?((?:tail |notail |musttail )?call )(.*)$' % NAME)
CALLEE = re.compile(r'(?:^|\s)(@(?:"[^"]+"|[-\w.$]+)|%s)\(' % NAME)
RET = re.compile(r'^(\s*)ret ptr (.+?)((?:, !\w+ !\d+)*)\s*$')
VALUE = re.compile(r'(?:^|\s)(%|@|null\b|inttoptr|getelementptr|undef\b|poison\b)')
BY_MEMORY = ("sret(", "byval(", "byref(", "inalloca(")


def pieces(text):
    out = []
    while text.strip():
        first, text = cut(text)
        out.append(first.strip())
    return out


def parameters(text):
    """[(kind, name)] of a definition's parameter list; kind is "ptr" only for a pointer passed as a value."""
    out = []
    for piece in pieces(text):
        words = piece.split()
        kind = words[0] if not any(mark in piece for mark in BY_MEMORY) else "memory"
        out.append((kind, words[-1] if words[-1].startswith("%") else None))
    return out


def signatures(text, norm):
    found = {}
    for line in text.split("\n"):
        m = DEFINE.match(line) if line.startswith("define ") else None
        if m:
            found[norm(m[1])] = [kind for kind, _ in parameters(m[2])]
    return found


def STRAY_BELOW(limit):
    """Every address below `limit` is nothing of the program's (the consoles: their memory starts far above)."""
    return "  %%s = icmp ult i32 %%p, %d\n  ret i1 %%s" % limit


def STRAY_RANGES(ranges):
    """The address is in one of the [low, high) ranges (the check build on a PC, whose own memory is anywhere)."""
    lines, last = [], "false"
    for i, (low, high) in enumerate(ranges):
        lines += ["  %%o%d = sub i32 %%p, %d" % (i, low), "  %%c%d = icmp ult i32 %%o%d, %d" % (i, i, high - low),
                  "  %%s%d = or i1 %s, %%c%d" % (i, last, i)]
        last = "%%s%d" % i
    return "\n".join(lines + ["  ret i1 " + last])


def helpers(stray):
    return """
@ss_arena = external global [%d x i8]
@ss_delta = external global [256 x i32]
@ss_back = external global [%d x i32]
@ss_scratch = external global [65536 x i8]
@ss_zeros = external constant [65536 x i8]

define internal i32 @ss.g2h(i32 %%g) alwaysinline nounwind {
  %%t = lshr i32 %%g, 24
  %%p = getelementptr inbounds [256 x i32], ptr @ss_delta, i32 0, i32 %%t
  %%d = load i32, ptr %%p, align 4
  %%h = add i32 %%g, %%d
  ret i32 %%h
}

define internal i32 @ss.h2g(i32 %%h) alwaysinline nounwind {
  %%o = sub i32 %%h, ptrtoint (ptr @ss_arena to i32)
  %%in = icmp ult i32 %%o, %d
  %%s = lshr i32 %%o, %d
  %%i = select i1 %%in, i32 %%s, i32 %d
  %%p = getelementptr inbounds [%d x i32], ptr @ss_back, i32 0, i32 %%i
  %%d = load i32, ptr %%p, align 4
  %%g = add i32 %%h, %%d
  ret i32 %%g
}

define internal i1 @ss.stray(i32 %%p) alwaysinline nounwind {
%s
}
""" % (ARENA, BACK, ARENA, SLOT.bit_length() - 1, BACK - 1, BACK, stray)


def arena_offset(address):
    """Where a GBA address lies in the block, or None for an address of no region."""
    address &= 0xFFFFFFFF
    top, low = address >> 24, address & 0xFFFFFF
    if top in REGIONS and low < SLOT:
        return (top - REGIONS[0]) * SLOT + low
    if top == 8:
        return ROM_AT + low
    return None


def cut(text):
    """`text` up to its first comma outside brackets, and what follows the comma."""
    depth = 0
    for i, ch in enumerate(text):
        depth += ch in "([{<"
        depth -= ch in ")]}>"
        if ch == "," and depth == 0:
            return text[:i], text[i + 1:].lstrip()
    return text, ""


def local(name):
    """A new value name made from an existing one (numbered values cannot be extended in place)."""
    return "%ss." + re.sub(r'[^-\w.$]', "_", name[1:])


def lower_function(lines, guard, counts, mine=None, game=None, norm=None):
    """mine: [(kind, name)] of this function's parameters when rule 5 covers it, else None."""
    game = game or {}
    arrived = {name for kind, name in mine or () if kind == "ptr" and name}
    # 1. local pointer variables nobody takes the address of
    pointers, others, uses = set(), set(), {}
    for line in lines:
        m = ALLOCA.match(line)
        if m:
            (pointers if m[2].strip() == "ptr" else others).add(m[1])
    for line in lines:
        if "llvm.dbg." in line or line.lstrip().startswith("#dbg_") or "llvm.lifetime." in line or ALLOCA.match(line):
            continue
        for name in set(re.findall(NAME, line)) & pointers:
            m = re.match(r'^\s*%s = load ptr, ptr (%s),' % (NAME, NAME), line)
            ok = bool(m and m[1] == name and re.findall(NAME, line).count(name) == 1)
            if not ok:
                m = re.match(r'^\s*store ptr (.*), ptr (%s), align' % NAME, line)
                ok = bool(m and m[2] == name and not re.search(re.escape(name) + r'(?![-\w.$])', m[1]))
            if not ok:
                uses[name] = True
    registers = {name for name in pointers if name not in uses}
    safe = set(pointers) | others           # addresses that are plainly the program's own: locals and what is cut from them
    out = []

    def guarded(indent, name, address, tail_holder):
        """Instructions that test `address`; returns (address to use, flag) or (address, None) when it needs no test."""
        if not guard or not address.startswith("%") or address in safe:
            return address, None
        base = local(name) + ".g%d" % len(out)
        out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, address))
        out.append("%s%s.s = call i1 @ss.stray(i32 %s.i)" % (indent, base, base))
        out.append("%s%s.p = select i1 %s.s, ptr @ss_scratch, ptr %s" % (indent, base, base, address))
        counts["guards"] += 1
        return base + ".p", base + ".s"

    for line in lines:
        m = GEP.match(line)
        if m:
            kind, rest = cut(m[2])
            base = cut(rest)[0].strip()
            if base.startswith("ptr "):
                base = base[4:].strip()
                if base in safe or base.startswith("@") or "@ss_arena" in base or (base.startswith("getelementptr") and "@" in base):
                    safe.add(m[1])
            out.append(line)
            continue
        m = LOAD.match(line)
        if m:
            indent, name, volatile, rest = m[1], m[2], m[3] or "", m[4]
            kind, rest = cut(rest)
            address, tail = cut(rest)
            if not address.startswith("ptr "):
                out.append(line)
                continue
            address, kind = address[4:].strip(), kind.strip()
            translate = kind == "ptr" and address not in registers
            use, flag = guarded(indent, name, address, None)
            if not translate and flag is None:
                out.append(line)
                continue
            base = local(name) + ".l%d" % len(out)
            raw = name if not translate and (flag is None or kind not in SCALAR) else base + ".raw"
            out.append("%s%s = load %s%s, ptr %s, %s" % (indent, raw, volatile, "i32" if translate else kind, use, tail))
            value = raw
            if flag is not None and (translate or kind in SCALAR):
                value = name if not translate else base + ".val"
                zero = "0" if translate or kind.startswith("i") else "null" if kind == "ptr" else "0.0"
                out.append("%s%s = select i1 %s, %s %s, %s %s" % (indent, value, flag, "i32" if translate else kind, zero,
                                                                 "i32" if translate else kind, raw))
            if translate:
                out.append("%s%s.h = call i32 @ss.g2h(i32 %s)" % (indent, base, value))
                out.append("%s%s = inttoptr i32 %s.h to ptr" % (indent, name, base))
                counts["loads"] += 1
            continue
        m = STORE.match(line)
        if m:
            indent, volatile, rest = m[1], m[2] or "", m[3]
            value, rest = cut(rest)
            address, tail = cut(rest)
            if not address.startswith("ptr "):
                out.append(line)
                continue
            address = address[4:].strip()
            translate = value.startswith("ptr ") and address not in registers
            use, flag = guarded(indent, "%st", address, None)
            if not translate and flag is None and not (value.startswith("ptr ") and value[4:].strip() in arrived):
                out.append(line)
                continue
            if value.startswith("ptr ") and value[4:].strip() in arrived:
                base = "%%ss.in.s%d" % len(out)         # rule 5: a pointer parameter, as the caller gave it
                out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, value[4:].strip()))
                out.append("%s%s.h = call i32 @ss.g2h(i32 %s.i)" % (indent, base, base))
                out.append("%s%s.p = inttoptr i32 %s.h to ptr" % (indent, base, base))
                value = "ptr %s.p" % base
                counts["parameters"] += 1
                if not translate and flag is None:
                    out.append("%sstore %s%s, ptr %s, %s" % (indent, volatile, value, use, tail))
                    continue
            if translate:
                value = value[4:].strip()
                fixed = CONST_ADDRESS.fullmatch(value)
                if value == "null":
                    value = "i32 0"
                elif fixed:
                    value = "i32 " + fixed[1]
                else:
                    base = "%%ss.st.s%d" % len(out)
                    out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, value))
                    out.append("%s%s.g = call i32 @ss.h2g(i32 %s.i)" % (indent, base, base))
                    value = "i32 %s.g" % base
                counts["stores"] += 1
            out.append("%sstore %s%s, ptr %s, %s" % (indent, volatile, value, use, tail))
            continue
        m = INTTOPTR.match(line)
        if m:
            indent, name, kind, value, tail = m.groups()
            base = local(name) + ".c%d" % len(out)
            if kind != "i32":
                out.append("%s%s.w = %s %s %s to i32" % (indent, base, "trunc" if int(kind[1:]) > 32 else "zext", kind, value))
                value = base + ".w"
            out.append("%s%s.h = call i32 @ss.g2h(i32 %s)" % (indent, base, value))
            out.append("%s%s = inttoptr i32 %s.h to ptr%s" % (indent, name, base, tail))
            counts["to pointer"] += 1
            continue
        m = PTRTOINT.match(line)
        if m:
            indent, name, value, kind, tail = m.groups()
            base = local(name) + ".c%d" % len(out)
            out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, value))
            if kind == "i32":
                out.append("%s%s = call i32 @ss.h2g(i32 %s.i)%s" % (indent, name, base, tail))
            else:
                out.append("%s%s.g = call i32 @ss.h2g(i32 %s.i)" % (indent, base, base))
                out.append("%s%s = %s i32 %s.g to %s%s" % (indent, name, "trunc" if int(kind[1:]) < 32 else "zext", base, kind, tail))
            counts["to number"] += 1
            continue
        m = RET.match(line) if mine is not None else None
        if m and not m[2].startswith(("null", "@")):
            base = "%%ss.ret.r%d" % len(out)
            out.append("%s%s.i = ptrtoint ptr %s to i32" % (m[1], base, m[2]))
            out.append("%s%s.g = call i32 @ss.h2g(i32 %s.i)" % (m[1], base, base))
            out.append("%s%s.p = inttoptr i32 %s.g to ptr" % (m[1], base, base))
            out.append("%sret ptr %s.p%s" % (m[1], base, m[3]))
            counts["results"] += 1
            continue
        m = CALL.match(line)
        callee = CALLEE.search(m[4]) if m else None
        if callee and not callee[1].startswith("@llvm."):
            indent, name, call, rest = m.groups()
            target = callee[1]
            known = game.get(norm(target[1:])) if target.startswith("@") else []
            if known is not None:
                before = rest[:callee.start(1)]
                end, depth = callee.end(), 1
                while depth:
                    depth += rest[end] == "("
                    depth -= rest[end] == ")"
                    end += 1
                given, after, changed = pieces(rest[callee.end():end - 1]), rest[end:], False
                for i, piece in enumerate(given):
                    if not piece.startswith("ptr") or any(mark in piece for mark in BY_MEMORY) or piece == "...":
                        continue
                    at = VALUE.search(piece)
                    if not at or piece[at.start(1):].startswith(("null", "@", "undef", "poison")):
                        continue
                    base = "%%ss.out.a%d.%d" % (len(out), i)
                    out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, piece[at.start(1):]))
                    out.append("%s%s.g = call i32 @ss.h2g(i32 %s.i)" % (indent, base, base))
                    out.append("%s%s.p = inttoptr i32 %s.g to ptr" % (indent, base, base))
                    given[i] = piece[:at.start(1)] + base + ".p"
                    changed = True
                    counts["arguments"] += 1
                returns = name is not None and before.split()[-1:] == ["ptr"]
                if changed or returns:
                    result = (local(name) + ".r%d" % len(out)) if returns else name
                    out.append("%s%s%s%s%s(%s)%s" % (indent, result + " = " if result else "", call, before, target, ", ".join(given), after))
                    if returns:
                        out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, result, result))
                        out.append("%s%s.h = call i32 @ss.g2h(i32 %s.i)" % (indent, result, result))
                        out.append("%s%s = inttoptr i32 %s.h to ptr" % (indent, name, result))
                        counts["results"] += 1
                    continue
        out.append(line)

    # 3. constant addresses; a constant pointer turned back into a number inside a larger constant is not handled
    final = []
    for line in out:
        if "inttoptr (" in line:
            line = FOLD.sub(lambda m: m[1], line)

            def place(m):
                offset = arena_offset(int(m[1]))
                if offset is None:
                    counts["constants outside every region"] += 1
                    return m[0]
                counts["constants"] += 1
                return "getelementptr (i8, ptr @ss_arena, i32 %d)" % offset
            line = CONST_ADDRESS.sub(place, line)
            at = line.find("ptrtoint (")
            while at >= 0:                  # the whole operand of each remaining constant conversion
                end, depth = at + 10, 1
                while depth:
                    depth += line[end] == "("
                    depth -= line[end] == ")"
                    end += 1
                if "@ss_arena" in line[at:end]:
                    counts["unhandled constant conversions"] += 1
                at = line.find("ptrtoint (", end)
        final.append(line)
    return final


GEP_CONST = re.compile(r'getelementptr (?:inbounds |nuw |nusw )*\(i8, ptr inttoptr \(i32 (-?\d+) to ptr\), i32 (-?\d+)\)')
COPIES = re.compile(r'^@(?:llvm\.mem(?:cpy|move|set)\.|"?(?:\\01)?_?mem(?:cpy|move|set)"?$)')
HOST_CALLS = re.compile(r'^@llvm\.mem(?:cpy|move|set)\.')


def lower_game_function(lines, counts, game, norm):
    """A function of a game module: see the head of this file."""
    safe, out = set(), []
    for line in lines:
        m = ALLOCA.match(line)
        if m:
            safe.add(m[1])

    def place(indent, address):
        """Instructions that find `address`; returns (address to use, stray flag or None)."""
        if address in safe or address.startswith("@"):
            return address, None
        fixed = CONST_ADDRESS.fullmatch(address)
        offset = arena_offset(int(fixed[1])) if fixed else None
        if not fixed:
            fixed = GEP_CONST.fullmatch(address)
            offset = arena_offset(int(fixed[1]) + int(fixed[2])) if fixed and arena_offset(int(fixed[1])) is not None else None
        if offset is not None:
            counts["constants"] += 1
            return "getelementptr (i8, ptr @ss_arena, i32 %d)" % offset, None
        if not address.startswith("%") and "inttoptr (" not in address:
            return address, None            # a constant cut from a global of the program
        base = "%%ss.at%d" % len(out)
        out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, address))
        out.append("%s%s.s = call i1 @ss.stray(i32 %s.i)" % (indent, base, base))
        out.append("%s%s.h = call i32 @ss.g2h(i32 %s.i)" % (indent, base, base))
        out.append("%s%s.q = inttoptr i32 %s.h to ptr" % (indent, base, base))
        out.append("%s%s.p = select i1 %s.s, ptr @ss_scratch, ptr %s.q" % (indent, base, base, base))
        counts["guards"] += 1
        return base + ".p", base + ".s"

    for line in lines:
        m = GEP.match(line)
        if m:
            base = cut(cut(m[2])[1])[0].strip()
            if base.startswith("ptr "):
                base = base[4:].strip()
                if base in safe or base.startswith("@") or (base.startswith("getelementptr") and "inttoptr (" not in base):
                    safe.add(m[1])
            out.append(line)
            continue
        m = LOAD.match(line)
        if m:
            indent, name, volatile, rest = m[1], m[2], m[3] or "", m[4]
            kind, rest = cut(rest)
            address, tail = cut(rest)
            kind = kind.strip()
            if not address.startswith("ptr "):
                out.append(line)
                continue
            use, flag = place(indent, address[4:].strip())
            if flag is None or kind not in SCALAR:
                out.append("%s%s = load %s%s, ptr %s, %s" % (indent, name, volatile, kind, use, tail))
                continue
            raw = local(name) + ".l%d" % len(out)
            out.append("%s%s = load %s%s, ptr %s, %s" % (indent, raw, volatile, kind, use, tail))
            zero = "0" if kind.startswith("i") else "null" if kind == "ptr" else "0.0"
            out.append("%s%s = select i1 %s, %s %s, %s %s" % (indent, name, flag, kind, zero, kind, raw))
            continue
        m = STORE.match(line)
        if m:
            indent, volatile, rest = m[1], m[2] or "", m[3]
            value, rest = cut(rest)
            address, tail = cut(rest)
            if not address.startswith("ptr "):
                out.append(line)
                continue
            use, flag = place(indent, address[4:].strip())
            out.append("%sstore %s%s, ptr %s, %s" % (indent, volatile, value, use, tail))
            continue
        m = CALL.match(line)
        callee = CALLEE.search(m[4]) if m else None
        if callee and (callee[1].startswith("%") or HOST_CALLS.match(callee[1]) or not callee[1].startswith("@llvm.")):
            indent, name, call, rest = m.groups()
            # a function that is not the game's is given real places; so is the copy the call itself makes of a
            # structure passed by value
            host = callee[1].startswith("@") and (HOST_CALLS.match(callee[1]) or norm(callee[1][1:]) not in game)
            end, depth = callee.end(), 1
            while depth:
                depth += rest[end] == "("
                depth -= rest[end] == ")"
                end += 1
            given, changed = pieces(rest[callee.end():end - 1]), False
            for i, piece in enumerate(given):
                copied = "byval(" in piece
                at = VALUE.search(piece) if piece.startswith("ptr") and (copied or (host and not any(mark in piece for mark in BY_MEMORY))) else None
                if copied and at:               # the type inside byval(...) is not the value
                    at = re.search(r'(%s|@\S+|inttoptr .*|getelementptr .*)$' % NAME, piece)
                    value = at[1] if at else None
                else:
                    value = piece[at.start(1):] if at else None
                if not value or value.startswith(("null", "@", "undef", "poison")) or value in safe:
                    continue
                piece_head = piece[:len(piece) - len(value)]
                base = "%%ss.out.a%d.%d" % (len(out), i)
                out.append("%s%s.i = ptrtoint ptr %s to i32" % (indent, base, value))
                out.append("%s%s.h = call i32 @ss.g2h(i32 %s.i)" % (indent, base, base))
                copies = COPIES.match(callee[1]) if not copied else None
                if copies and i < 2:
                    # a block copy through an address the console would have ignored: the write is lost, the read is zeros
                    out.append("%s%s.q = inttoptr i32 %s.h to ptr" % (indent, base, base))
                    out.append("%s%s.s = call i1 @ss.stray(i32 %s.i)" % (indent, base, base))
                    out.append("%s%s.p = select i1 %s.s, ptr %s, ptr %s.q" % (indent, base, base, "@ss_scratch" if i == 0 else "@ss_zeros", base))
                else:
                    out.append("%s%s.p = inttoptr i32 %s.h to ptr" % (indent, base, base))
                given[i] = piece_head + base + ".p"
                changed = True
                counts["arguments"] += 1
            if changed:
                out.append("%s%s%s%s(%s)%s" % (indent, name + " = " if name else "", call, rest[:callee.end() - 1], ", ".join(given), rest[end:]))
                continue
        out.append(line)
    return out


def lower(text, guard, stray, game=None, norm=lambda name: name):
    counts = {"loads": 0, "stores": 0, "to pointer": 0, "to number": 0, "constants": 0, "guards": 0, "parameters": 0,
              "arguments": 0, "results": 0, "constants outside every region": 0, "unhandled constant conversions": 0}
    game = game or {}
    out, body, mine = [], None, None
    for line in text.split("\n"):
        if body is None:
            out.append(line)
            if line.startswith("define ") and line.rstrip().endswith("{"):
                body = []
                m = DEFINE.match(line)
                if not m:
                    raise SystemExit("rebase_ir: cannot read this definition: " + line[:200])
                mine = parameters(m[2]) if norm(m[1]) in game else None
        elif line == "}":
            out += lower_game_function(body, counts, game, norm) if guard else lower_function(body, False, counts, mine, game, norm)
            out.append(line)
            body = None
        else:
            body.append(line)
    if counts["unhandled constant conversions"]:
        raise SystemExit("rebase_ir: a constant GBA address is turned back into a number inside a larger constant; "
                         "teach lower_function that form")
    return "\n".join(out) + helpers(stray), counts
