#!/usr/bin/env python3
"""check_register_args.py --elf shiningsoul2_eu.elf [--game ss2] : calls whose C passes fewer argument registers
than the cartridge's callee reads, and calls whose C ignores a result the cartridge's caller uses.

check_calls.py and check_decls.py compare the C with the C. Here the reference is the machine code of the
matching build (its ELF, which has function symbols and $t/$a/$d mapping symbols):

  reads      for every function, the argument registers r0-r3 it reads before writing them, on any path,
             through the functions it calls and jumps to (fixed point over the whole program)
  results    for every BL, whether the caller reads r0 after the call before writing it

Then every call in the generated C (build.py prepare) is resolved to an address (the function's name in the
ELF, a sub_XXXXXXXX name, or an asm("...") alias) and compared:

  short call    the C passes n arguments, the callee reads r_k with k >= n
  short def     the C definition has n parameters, the machine code reads r_k with k >= n
  lost result   the C calls a function declared void (or drops the value) and the cartridge's caller of
                that function reads r0 after one of its BLs to it

A finding is a lead, not a bug: a read can be a spill of a register that is never used, and a C call with
fewer arguments may be one where the cartridge's value is irrelevant. Read the caller's disassembly at the
BL (printed with --show) to see what value the cartridge leaves there. The ELF is not in this repository:
build the decompilation (games/ss2/decomp) with its own toolchain.
"""
import argparse, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ARG = ("r0", "r1", "r2", "r3")
SWI_READS = {0: 0, 1: 1, 2: 0, 3: 0, 4: 2, 5: 0, 6: 2, 7: 2, 8: 1, 9: 1, 0xA: 2, 0xB: 3, 0xC: 3, 0xE: 3,
             0xF: 4, 0x10: 3, 0x11: 2, 0x12: 2, 0x13: 2, 0x14: 2, 0x15: 2, 0x16: 2, 0x17: 2, 0x18: 2, 0x19: 1,
             0x1F: 3}


class Program:
    def __init__(self, elf_path):
        from elftools.elf.elffile import ELFFile
        import capstone
        self.cs = {m: capstone.Cs(capstone.CS_ARCH_ARM, m) for m in (capstone.CS_MODE_THUMB, capstone.CS_MODE_ARM)}
        for c in self.cs.values():
            c.detail = True
        self.THUMB, self.ARMM = capstone.CS_MODE_THUMB, capstone.CS_MODE_ARM
        with open(elf_path, "rb") as fh:
            elf = ELFFile(fh)
            text = elf.get_section_by_name(".text")
            self.base, self.data = text["sh_addr"], text.data()
            self.funcs, self.names, self.addr_of, maps = {}, {}, {}, []
            for s in elf.get_section_by_name(".symtab").iter_symbols():
                v, t = s["st_value"], s["st_info"]["type"]
                if not self.base <= v & ~1 < self.base + len(self.data):
                    continue
                if t == "STT_FUNC":
                    a = v & ~1
                    self.funcs[a] = self.THUMB if v & 1 else self.ARMM
                    self.names.setdefault(a, s.name)
                    self.addr_of[s.name] = a
                elif s.name in ("$t", "$a", "$d"):
                    maps.append((v, s.name))
        self.starts = sorted(self.funcs)
        self.end = {a: b for a, b in zip(self.starts, self.starts[1:] + [self.base + len(self.data)])}
        self.datamap = sorted(maps)
        self.decoded = {}

    def word(self, a):
        o = a - self.base
        return struct.unpack_from("<I", self.data, o)[0] if 0 <= o <= len(self.data) - 4 else None

    def insn(self, a, mode):
        key = (a, mode)
        if key not in self.decoded:
            o = a - self.base
            if not 0 <= o < len(self.data):
                self.decoded[key] = None
            else:
                got = next(self.cs[mode].disasm(self.data[o:o + 4], a, 1), None)
                self.decoded[key] = got
        return self.decoded[key]

    def is_data(self, a):
        import bisect
        i = bisect.bisect_right(self.datamap, (a, "~")) - 1
        return i >= 0 and self.datamap[i][1] == "$d"

    def switch_table(self, a, mode):
        size = 2 if mode == self.THUMB else 4
        for back in range(1, 9):
            i = self.insn(a - back * size, mode)
            if i and i.mnemonic == "ldr" and "[pc" in i.op_str:
                lit = ((i.address + (4 if mode == self.THUMB else 8)) & ~3) + i.operands[1].mem.disp
                return self.word(lit)
        return None

    def function_of(self, a):
        import bisect
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None


def regs(insn):
    r, w = insn.regs_access()
    return {insn.reg_name(x) for x in r}, {insn.reg_name(x) for x in w}


def analyse(prog, reads, func, record=None, trace=None, strict=True):
    """Argument registers `func` reads before writing. record(site, callee, insn) gets every BL.

    strict: a read counts only where no path to it has written the register. Arguments are read like that;
    an uninitialised local that only one impossible path leaves unwritten (a switch without a default) is not."""
    mode, start, end = prog.funcs[func], func, prog.end[func]
    state = {start: frozenset()}          # address -> argument registers written on every (strict: any) path
    work, used, reads_at = [start], set(), {}
    while work:
        a = work.pop()
        written = state[a]
        i = None if prog.is_data(a) else prog.insn(a, mode)
        if i is None:
            continue
        m = i.mnemonic
        succ, rd, wr = [], set(), set()
        nxt = a + i.size
        ops = i.operands
        cond = mode == prog.ARMM and i.cc not in (0, 15)
        if m == "bl" or m == "blx":
            tgt = ops[0].imm if ops and ops[0].type == 2 else None
            if tgt is not None and start <= tgt < end and tgt != start:
                succ = [tgt]
            else:
                if tgt in prog.funcs:
                    rd = {ARG[k] for k in reads.get(tgt, ())}
                    if record:
                        record(a, tgt, i)
                elif tgt is None:
                    rd = regs(i)[0]
                wr = set(ARG)
                succ = [nxt]
        elif m in ("push", "stmdb") and (m == "push" or i.reg_name(ops[0].reg) == "sp"):
            succ = [nxt]
        elif m in ("b",) or (m.startswith("b") and len(m) == 3 and m not in ("bic", "bkpt")) or m in ("beq.w",):
            tgt = ops[0].imm
            if start <= tgt < end:
                succ = [tgt]
            elif tgt in prog.funcs:
                rd = {ARG[k] for k in reads.get(tgt, ())}
            if m != "b" or cond:
                succ.append(nxt)
        elif m == "bx":
            r = i.reg_name(ops[0].reg)
            rd = {r} if r != "lr" else set()
        elif m == "swi" or m == "svc":
            n = ops[0].imm & 0xFF if mode == prog.THUMB else (ops[0].imm >> 16) & 0xFF
            rd = set(ARG[:SWI_READS.get(n, 4)])
            wr = {"r0", "r1", "r3"}
            succ = [nxt]
        elif m == "pop" or (m == "ldm" and "pc" in [i.reg_name(o.reg) for o in ops[1:]]) or m == "ldmia" and False:
            names = [i.reg_name(o.reg) for o in ops]
            wr = set(names)
            if "pc" not in names:
                succ = [nxt]
        else:
            rd, wr = regs(i)
            if "pc" in wr and m in ("mov", "add", "ldr"):
                # agbcc switch: the table's address is loaded from the literal pool a few instructions before
                t = prog.switch_table(a, mode)
                while t is not None and start <= t < end:
                    w = prog.word(t)
                    if w is None or not start <= w < end:
                        break
                    succ.append(w & ~1)
                    t += 4
            elif not (cond and False):
                succ = [nxt]
            if cond:
                succ = [nxt] if "pc" not in wr else succ + [nxt]
        reads_at[a] = ({r for r in rd if r in ARG}, "%s %s" % (m, i.op_str))
        if cond and not strict:
            wr = set()
        after = written | frozenset(r for r in wr if r in ARG)
        for s in succ:
            if not start <= s < end:
                continue
            old = state.get(s)
            new = after if old is None else (old | after) if strict else (old & after)
            if new != old:
                state[s] = new
                work.append(s)
    for a in sorted(reads_at):             # after the fixed point: every path has been merged into state
        rd, text = reads_at[a]
        new_reads = {r for r in rd if r not in state[a]} - used
        if trace is not None and new_reads:
            trace.append((a, sorted(new_reads), text))
        used |= new_reads
    return frozenset(ARG.index(r) for r in used)


def all_reads(prog, strict=True):
    reads = {}
    changed = True
    while changed:
        changed = False
        for f in prog.starts:
            r = analyse(prog, reads, f, strict=strict)
            if r != reads.get(f):
                reads[f] = r
                changed = True
    return reads


def result_users(prog, reads):
    """{(caller, callee): [BL addresses after which the caller reads r0 before writing it]}"""
    sites = []
    for f in prog.starts:
        analyse(prog, reads, f, lambda site, callee, insn, f=f: sites.append((f, site, callee, insn.size)))
    users = {}
    for f, site, callee, size in sites:
        if r0_used_after(prog, reads, f, site + size):
            users.setdefault((f, callee), []).append(site)
    return users


def r0_used_after(prog, reads, func, start, limit=400):
    mode, lo, hi = prog.funcs[func], func, prog.end[func]
    seen, work = set(), [start]
    while work and len(seen) < limit:
        a = work.pop()
        if a in seen or not lo <= a < hi:
            continue
        seen.add(a)
        i = prog.insn(a, mode)
        if i is None:
            continue
        m, ops, nxt = i.mnemonic, i.operands, a + i.size
        if m in ("bl", "blx"):
            tgt = ops[0].imm if ops and ops[0].type == 2 else None
            if tgt in prog.funcs and 0 in reads.get(tgt, ()):
                return True
            continue                      # r0 clobbered by the call
        if m in ("push",):
            work.append(nxt)
            continue
        if m == "pop":
            names = [i.reg_name(o.reg) for o in ops]
            if "r0" in names:
                continue
            if "pc" in names:
                continue                  # returned: not counted as a use here
            work.append(nxt)
            continue
        if m == "bx":
            continue
        if m.startswith("b") and m not in ("bic", "bics", "bkpt") and ops and ops[0].type == 2:
            work.append(ops[0].imm)
            if m != "b":
                work.append(nxt)
            continue
        rd, wr = regs(i)
        if "r0" in rd:
            return True
        if "r0" in wr:
            continue
        work.append(nxt)
    return False


# ---- C side -------------------------------------------------------------------------------------------------

ARGS = r'\(((?:[^;{}()]|\([^()]*\))*)\)'
DEFINITION = re.compile(r'^(?![ \t#/])([^\n;{}#=]*?)\b(\w+)\s*%s\s*\{' % ARGS, re.M)
DECLARATION = re.compile(r'(?:^|[;{}])\s*(?:extern\s+|static\s+)*([A-Za-z_][\w \t\*]*?[ \t\*])(\w+)\s*%s\s*'
                         r'((?:__asm__|asm)\s*\(\s*"_?(\w+)"\s*\)\s*)?;' % ARGS, re.M)
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do"}


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


def by_value(p):
    p = p.strip()
    return bool(re.match(r'(?:const\s+)?(?:struct|union)\s+\w+', p)) and not any(c in p for c in "*[(")


def call_args(text, i):
    """Arguments of the call whose '(' is at i, as strings, and the index after ')'."""
    depth, cur, out = 0, "", []
    while i < len(text):
        ch = text[i]
        if ch in "([{":
            depth += 1
            if depth > 1:
                cur += ch
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                if cur.strip():
                    out.append(cur)
                return out, i + 1
            cur += ch
        elif ch == "," and depth == 1:
            out.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    return None, i


def resolve(prog, name, aliases):
    name = aliases.get(name, name)
    if name in prog.addr_of:
        return prog.addr_of[name]
    m = re.fullmatch(r'(?:sub|thunk|nullsub|Func)_?(0[89][0-9A-Fa-f]{6})\w*', name)
    if m and int(m[1], 16) in prog.funcs:
        return int(m[1], 16)
    return None


CAST_CALL = re.compile(r'\(\s*\([^;{}()]*\(\s*\*\s*\)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*\)\s*&?\s*(\w+)\s*\)\s*\(')


class Call:
    """A call found in the text: [1] is the callee's name, end() is just after its '('."""
    def __init__(self, name, start, end, cast=None):
        self.name, self._start, self._end, self.cast = name, start, end, cast

    def __getitem__(self, k):
        return self.name

    def start(self):
        return self._start

    def end(self):
        return self._end


def calls_in(body):
    """Plain calls `f(...)` and calls through a cast of the function, `((T (*)(P))f)(...)`."""
    for m in re.finditer(r'\b(\w+)\s*\(', body):
        yield Call(m[1], m.start(), m.end())
    for m in CAST_CALL.finditer(body):
        yield Call(m[2], m.start(), m.end(), cast=split(m[1]))


def scan_c(prog, reads, users, files):
    findings, used_in_c, dropped = [], {}, {}
    for path, text in files.items():
        aliases, decl_params, decl_void = {}, {}, {}
        for m in DECLARATION.finditer(text):
            if m[1].strip() in ("return", "else", "goto", "case"):
                continue
            if m[5]:
                aliases[m[2]] = m[5]
            decl_params[m[2]] = split(m[3])
            decl_void[m[2]] = re.fullmatch(r'\s*(?:extern\s+|static\s+)*void\s*', m[1]) is not None
        defs = [(m.start(), m[2], m[1], m[3]) for m in DEFINITION.finditer(text) if m[2] not in KEYWORDS]
        for k, (pos, fname, ret, params) in enumerate(defs):
            body_end = defs[k + 1][0] if k + 1 < len(defs) else len(text)
            body_start = text.index("{", pos)
            caller = resolve(prog, fname, aliases)
            p = split(params)
            if caller is not None and p is not None and not any(by_value(x) for x in p) and "..." not in params:
                need = max(reads.get(caller, ()), default=-1) + 1
                if need > len(p):
                    findings.append(("short def", path.name, text.count("\n", 0, pos) + 1, fname, caller, None,
                                     len(p), need, ""))
            for m in calls_in(text[body_start:body_end]):
                name = m[1]
                if name in KEYWORDS:
                    continue
                callee = resolve(prog, name, aliases)
                if callee is None:
                    continue
                at = body_start + m.end() - 1
                args, after = call_args(text, at)
                if args is None:
                    continue
                line_start = text.rfind("\n", 0, body_start + m.start()) + 1
                before = text[line_start:body_start + m.start()]
                if re.search(r'\b(?:extern|void|u8|u16|u32|s8|s16|s32|int|struct\s+\w+|static|asm)\s*\**\s*$', before):
                    continue
                if "asm(" in text[after:text.find("\n", after)] or "__asm__" in text[after:text.find("\n", after)]:
                    continue
                declared = m.cast if m.cast is not None else decl_params.get(name)
                if declared and any(by_value(x) for x in declared):
                    continue
                if any(re.match(r'\s*\*\s*\(\s*(?:struct|union)\s+\w+\s*\*\s*\)', a) for a in args):
                    continue
                line = text.count("\n", 0, at) + 1
                src = text[line_start:text.find("\n", at)].strip()[:120]
                need = max(reads.get(callee, ()), default=-1) + 1
                if len(args) < need:
                    findings.append(("short call", path.name, line, fname, caller, callee, len(args), need, src))
                # result dropped: statement call, or declared void
                stmt = re.match(r'\s*;', text[after:]) and re.search(
                    r'(?:^|[;{}:]|\belse|\(void\))\s*$', text[max(0, body_start + m.start() - 60):body_start + m.start()])
                if caller is not None and (caller, callee) in users:
                    if decl_void.get(name) or stmt:
                        dropped.setdefault((caller, callee), []).append((path.name, line, fname, src))
                    else:
                        used_in_c[(caller, callee)] = used_in_c.get((caller, callee), 0) + 1
    # a caller may call the same function several times and use only some results: compare the counts
    for key, lines in dropped.items():
        if used_in_c.get(key, 0) < len(users[key]):
            for fn, line, fname, src in lines:
                findings.append(("lost result", fn, line, fname, key[0], key[1], used_in_c.get(key, 0),
                                 len(users[key]), src))
    return findings


def main():
    import build
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", required=True, help="ELF of the matching decompilation build")
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("--variant", default="gen")
    parser.add_argument("--sources", help="scan this folder of C files instead of the generated sources")
    parser.add_argument("--loose", action="store_true",
                        help="count a read that only some paths reach unwritten (finds uninitialised locals too)")
    parser.add_argument("--show", action="store_true", help="print the caller's instructions before each BL")
    args = parser.parse_args()
    prog = Program(args.elf)
    reads = all_reads(prog, strict=not args.loose)
    users = result_users(prog, reads)
    gen = Path(args.sources) if args.sources else build.ROOT / "build" / args.variant / args.game / "src"
    replaced = build.port_config(args.game)["runtime"]
    files = {p: p.read_text(encoding="utf-8", errors="replace") for p in sorted(gen.glob("*.c")) if p.stem not in replaced}
    if not files:
        parser.error(f"No generated modules in {gen}. Run build.py prepare first.")
    findings = scan_c(prog, reads, users, files)
    seen = set()
    for kind, fn, line, cname, caller, callee, given, need, src in findings:
        key = (kind, fn, line, callee)
        if key in seen:
            continue
        seen.add(key)
        cal = "%08X" % caller if caller else "?"
        tgt = "%s %08X" % (prog.names.get(callee, "?"), callee) if callee else ""
        print("%-11s %s:%d %s(%s) -> %s: C %d, reads r0..r%d   %s" % (kind, fn, line, cname, cal, tgt, given, need - 1, src))
        if args.show and callee and caller:
            show(prog, caller, callee)
    print("%d finding(s)" % len(seen))
    return 0


def show(prog, caller, callee):
    mode, a, end = prog.funcs[caller], caller, prog.end[caller]
    hist = []
    while a < end:
        i = prog.insn(a, mode)
        if i is None:
            a += 2
            continue
        hist.append(i)
        if i.mnemonic == "bl" and i.operands[0].imm == callee:
            for h in hist[-8:]:
                print("        %08X  %s %s" % (h.address, h.mnemonic, h.op_str))
        a += i.size


if __name__ == "__main__":
    sys.exit(main())
