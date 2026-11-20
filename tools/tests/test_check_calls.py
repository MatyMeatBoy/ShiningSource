"""The ABI audit counts C operands without confusing macros with asm aliases."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from check_calls import ALIAS, count_arguments


class CheckCalls(unittest.TestCase):
    def test_nested_casts_are_arguments(self):
        self.assertEqual(count_arguments("((u32 *)(actor + 12))", 0), 1)
        self.assertEqual(count_arguments("((Vec){1, 2}, table[index + 1])", 0), 2)
        self.assertEqual(count_arguments("()", 0), 0)

    def test_alias_starts_at_declaration(self):
        code = '''#define FIELD(p, n) ((u8 *)(p))
extern void named(u8 *, u32) asm("sub_08012300");
'''
        self.assertEqual(ALIAS.findall(code), [("named", "sub_08012300")])
