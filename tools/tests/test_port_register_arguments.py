"""Exercise native calls that take live ARM arguments in the original cartridge."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import build


def extract(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + len("\n}")
    return source[start:end]


class PortRegisterArgumentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_folder = build.ROOT / "build"
        build_folder.mkdir(exist_ok=True)
        scratch = tempfile.TemporaryDirectory(prefix="register-test-", dir=build_folder)
        cls.addClassCleanup(scratch.cleanup)
        cls.generated = build.prepare("ss2", prefix=False, variant=Path(scratch.name).name) / "src"

    def source(self, filename):
        return (self.generated / filename).read_text(encoding="utf-8")

    def test_select_weapon_passes_the_selected_slot(self):
        helper = extract(self.source("code_08058790.c"), "void sub_08058C74(")
        self.run_harness(r"""
#include <assert.h>
typedef unsigned char u8;
typedef short s16;
typedef int s32;
typedef unsigned int u32;
#define M2C_FIELD(base, type, offset) (*(type)((base) + (offset)))

static _Alignas(2) u8 inventory[0x80];
static u8 *gUnk_030034E0 = inventory;

s32 sub_0805AD78(u32 slot)
{
    assert(slot == 2);
    assert(M2C_FIELD(inventory, s16 *, 0x68) == 2);
    return 0;
}

@SUBJECT@

int main(void)
{
    sub_08058C74(2);
}
""".replace("@SUBJECT@", helper))

    def test_field_item_removal_passes_key_and_enqueues_found_item(self):
        helper = extract(self.source("code_08057CAC.c"), "void sub_080585F4(")
        self.run_harness(r"""
#include <assert.h>
typedef unsigned char u8;
typedef int s32;
typedef unsigned int u32;

static u8 item_id;
static int removals;

void *sub_0805D37C(s32 x, s32 y, s32 key)
{
    assert(x == -2 * 256 && y == 5 * 256 && key == 6);
    return &item_id;
}

u32 sub_080687A8(const u8 *item)
{
    assert(item == &item_id);
    removals++;
    return 1;
}

@SUBJECT@

int main(void)
{
    sub_080585F4(-2, 5, 6);
    assert(removals == 1);
}
""".replace("@SUBJECT@", helper))

    def test_cutscenes_send_the_found_field_item_to_the_removal_queue(self):
        for filename, count in (("code_080443EC.c", 1), ("code_0804D330.c", 5)):
            with self.subTest(filename=filename):
                source = self.source(filename)
                self.assertIsNone(re.search(r"\bsub_080687A8\(\)", source))
                self.assertEqual(len(re.findall(r"sub_080687A8\(sub_0805D37C\([^;]+\)\)", source)), count)

    def test_actor_scaling_passes_the_stat_block(self):
        for filename, function, field in (
            ("code_0808D4A8.c", "resetF1E4", "data"),
            ("code_08097E28.c", "s19F60", "sub"),
        ):
            with self.subTest(filename=filename):
                source = self.source(filename)
                call = re.search(rf"^    {function}\(.*?\);$", source, re.M)[0]
                self.run_harness(r"""
#include <assert.h>
typedef short s16;
static s16 stats[] = {10, 20, 30, 40};
static void @FUNCTION@(s16 *actor_stats)
{
    assert(actor_stats == stats);
    actor_stats[3] *= 2;
}
int main(void)
{
    void *@FIELD@ = stats;
@CALL@
    assert(stats[0] == 10 && stats[3] == 80);
}
""".replace("@FUNCTION@", function).replace("@FIELD@", field).replace("@CALL@", call))

    def test_reset_passes_actor_index(self):
        helper = extract(self.source("code_08057CAC.c"), "void sub_0805874C(")
        self.run_harness(r"""
#include <assert.h>
typedef unsigned int u32;

static unsigned int reset_index;

void sub_0801A388(u32 index)
{
    reset_index = index;
}

@SUBJECT@

int main(void)
{
    sub_0805874C(3);
    assert(reset_index == 3);
}
""".replace("@SUBJECT@", helper))

    def test_worm_segments_take_their_own_layout_entries(self):
        # AU2P 080AD54E-080AD576: sub_080AD534 leaves idx + 1 in r2 for the actor setup (sub_0801623C reads it as
        # the layout entry); without it the desert worm's segments were set up from whatever the register held.
        segment = extract(self.source("code_080A9680.c"), "void sub_080AD534_v(u32 idx)")
        self.assertIn("setupAD534(actorsAD534[idx], imageAD534, idx + 1);", segment)
        self.assertIn("void setupAD534(void *, void *, s32) asm(", self.source("code_080A9680.c"))

    def test_stage_exit_and_bank_regeneration_forward_the_game_state(self):
        source = self.source("code_08021DC4.c")
        exit_handler = extract(source, "s32 sub_08027B50(")
        self.assertIn("default:\n            sub_08057BD0(gUnk_03003170.curLevel);", exit_handler)
        source = self.source("code_0805FB7C.c")
        self.assertIn("sub_0805C7B8(sub_08057AE8(0x15));", source)
        source = self.source("code_08009088.c")
        self.assertIn("if (sub_0801AAF8() == 0) {\n        sub_0801ABE8(0);", source)

    def run_harness(self, source):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness = root / "register_arguments.c"
            binary = root / "register_arguments.exe"
            harness.write_text(source, encoding="utf-8")
            subprocess.run([build.clang(), "-std=gnu11", "-Werror", str(harness), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
