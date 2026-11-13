#!/usr/bin/env python3
"""Give integer division the cartridge's results on processors that do not trap.

The cartridge divides with the compiler's library routines. Their contract (read from the ROM's own __divsi3,
__modsi3, __udivsi3 and __umodsi3): a zero divisor gives quotient 0 and remainder 0; the most negative number
divided by -1 gives itself with remainder 0. In C both cases are undefined, and each processor does something
else: x86 traps (the fault handlers of the hosts return the cartridge's values), ARM64 gives quotient 0 but the
dividend as remainder, WebAssembly traps and cannot be resumed.

For the targets that do not trap, the game modules are compiled to LLVM IR before any optimisation, this module
rewrites the division instructions so that the cartridge's result is computed explicitly, and the IR is then
compiled as usual. Doing it before optimisation matters: afterwards the compiler may already have used the
undefined cases to remove code.

  lower(text, "rem")   guard remainders by zero (ARM64: its quotients already agree)
  lower(text, "all")   guard quotients and remainders, zero divisors and the signed overflow (WebAssembly)
"""
import re

INSTRUCTION = re.compile(r'^(\s*)(%[\w.]+) = (sdiv|udiv|srem|urem)( exact)? (i8|i16|i32|i64) ([^,]+), ([^,\n]+?)((?:, !\w+ !\d+)*)\s*$')


def lower(text, mode):
    out, count = [], 0
    for line in text.split("\n"):
        m = INSTRUCTION.match(line)
        if not m:
            out.append(line)
            continue
        indent, result, op, exact, bits, a, b, tail = m.groups()
        remainder, signed = op.endswith("rem"), op.startswith("s")
        literal = re.fullmatch(r'-?\d+', b.strip())
        zero_possible = not literal or int(b) == 0
        overflow_possible = signed and (not literal or int(b) == -1)
        guard_zero = zero_possible and (remainder or mode == "all")
        guard_overflow = overflow_possible and mode == "all"
        if not guard_zero and not guard_overflow:
            out.append(line)
            continue
        count += 1
        t = result[1:].strip('"') + ".div%d" % count
        name = lambda suffix: '%%"%s.%s"' % (t, suffix)
        divisor, conditions = b, []
        if guard_zero:
            out.append("%s%s = icmp eq %s %s, 0" % (indent, name("z"), bits, b))
            conditions.append(name("z"))
        if guard_overflow:
            minimum = {"i8": "-128", "i16": "-32768", "i32": "-2147483648", "i64": "-9223372036854775808"}[bits]
            out.append("%s%s = icmp eq %s %s, %s" % (indent, name("m"), bits, a, minimum))
            out.append("%s%s = icmp eq %s %s, -1" % (indent, name("n"), bits, b))
            out.append("%s%s = and i1 %s, %s" % (indent, name("o"), name("m"), name("n")))
            conditions.append(name("o"))
        if len(conditions) == 2:
            out.append("%s%s = or i1 %s, %s" % (indent, name("c"), conditions[0], conditions[1]))
            bad = name("c")
        else:
            bad = conditions[0]
        out.append("%s%s = select i1 %s, %s 1, %s %s" % (indent, name("d"), bad, bits, bits, b))
        out.append("%s%s = %s %s %s, %s%s" % (indent, name("r"), op, bits, a, name("d"), tail))
        # what the cartridge returns in the guarded cases: 0, except the overflowing quotient, which is the dividend
        special = "0"
        if guard_overflow and not remainder:
            if guard_zero:
                out.append("%s%s = select i1 %s, %s %s, %s 0" % (indent, name("s"), name("o"), bits, a, bits))
                special = name("s")
            else:
                special = a
        out.append("%s%s = select i1 %s, %s %s, %s %s" % (indent, result, bad, bits, special, bits, name("r")))
    return "\n".join(out), count
