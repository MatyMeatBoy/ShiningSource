"""Exercise the ported weapon selection predicate with distinct item and skill IDs."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import build, prepared_source


HARNESS = r"""
#include <assert.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

static _Alignas(2) u8 weapon[0xB0];
static u32 player_class;
static int rank_checks;
static u16 ranks[10];
static void *sub_08007290(u32 item) {
    assert(item == 0xB0003);
    return weapon;
}
static u32 sub_0801DE60(void) { return player_class; }
s32 sub_0801D028(u16 subtype) {
    rank_checks++;
    assert(subtype == 1);
    return ranks[subtype];
}

@PREDICATE@

int main(void) {
    _Alignas(2) u8 item[16] = {2, 0, 3, 0};
    weapon[0x9B] = 1;
    weapon[0xAC] = 1;
    weapon[0xAE] = 1; /* Only class zero can select this weapon. */

    ranks[3] = 1; /* Weapon ID matches an unrelated skill at the required rank. */
    assert(!sub_0805AE34(item));
    ranks[1] = 1;
    ranks[3] = 0;
    assert(sub_0805AE34(item));
    assert(rank_checks == 2);

    player_class = 1;
    assert(!sub_0805AE34(item));
    player_class = 0;
    weapon[0x9B] = 9;
    assert(sub_0805AE34(item));
    item[8] = 1;
    assert(!sub_0805AE34(item));
    assert(rank_checks == 2);
    return 0;
}
"""


class WeaponSkillTests(unittest.TestCase):
    def test_player_initialization_restores_only_an_already_active_weapon(self):
        source = prepared_source("code_0805A630.c")
        start = source.index("void ss_restore_selected_weapon(const u8 *save)\n{")
        end = source.index("\n}", start) + len("\n}")
        harness = """#include <assert.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
static u32 activations, activated_slot;
static int impl_0805AD78(u32 slot) {
    activations++;
    activated_slot = slot;
    return 0;
}
@RESTORE@
int main(void) {
    _Alignas(2) u8 save[0xa0] = {0};
    save[0x70] = 2;
    ss_restore_selected_weapon(save);
    assert(activations == 0);
    save[0x79] = 1;
    save[0x68] = 3;
    ss_restore_selected_weapon(save);
    assert(activations == 0);
    save[0x68] = 1;
    save[0x89] = 1;
    ss_restore_selected_weapon(save);
    assert(activations == 0);
    save[0x80] = 2;
    save[0x88] = 1;
    ss_restore_selected_weapon(save);
    assert(activations == 0);
    save[0x88] = 0;
    ss_restore_selected_weapon(save);
    assert(activations == 1 && activated_slot == 1);
    return 0;
}
"""
        self.run_harness(harness.replace("@RESTORE@", source[start:end]))

    def test_selection_checks_the_weapons_subtype_skill(self):
        source = prepared_source("code_0805A630.c")
        start = source.index("u32 sub_0805AE34(const u8 *item)\n{")
        end = source.index("\n}", start) + len("\n}")
        predicate = build.ARM_PIN.sub(lambda match: match[1], source[start:end])
        self.run_harness(HARNESS.replace("@PREDICATE@", predicate))

    def run_harness(self, source):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            harness = path / "weapon_skill.c"
            harness.write_text(source)
            binary = path / "weapon_skill.exe"
            subprocess.run([build.clang(), "-std=gnu11", str(harness), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
