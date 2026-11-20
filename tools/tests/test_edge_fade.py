"""Exercise the real pixel compositor without a ROM."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <windows.h>
#include <stdio.h>
#include "runtime/core/ppu.c"
unsigned long ss_frame_number(void) { return 0; }
const uint8_t *ss_line_registers(int y) { (void)y; return NULL; }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    struct obj_line obj = {0};
    static uint16_t oam[512], palette[256];
    static uint8_t tiles[32768];
    uint16_t entry[4] = {52, 0, 0, 0};
    int moved;
    CHECK(VirtualAlloc((void *)(uintptr_t)SS_PALETTE, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(VirtualAlloc((void *)(uintptr_t)SS_IO, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    *(uint16_t *)(uintptr_t)(SS_PALETTE + 2) = 0x7FFF;
    view.world = 1; view.width = 480; view.shift = 120;
    view.hud_left = 120; view.hud_right = 360;
    prepare_edge_fade(); line.dispcnt = 0x1000;
    obj.colour[120] = 2; obj.prio[120] = 0;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* player, priority zero */
    obj.colour[119] = 2;
    CHECK(compose(119, 80, &obj, &moved) == 0); /* no sprite leaks into black */
    obj.colour[359] = 2;
    CHECK(compose(359, 80, &obj, &moved) == 0); /* right edge */
    obj.colour[128] = 2; obj.prio[128] = 1;
    CHECK(compose(128, 80, &obj, &moved) == 0x7FFF); /* only eight columns */
    CHECK(compose(120, 4, &obj, &moved) == 0); /* world sprite crossing HUD band */
    obj.flags[120] = 8;
    CHECK(compose(120, 4, &obj, &moved) == 0x7FFF); /* tagged HUD sprite */
    page_shown = 1;
    CHECK(compose(120, 80, &obj, &moved) == 0x7FFF); /* Start/items/status */
    obj.flags[120] = 0;
    page_shown = 0;
    obj.colour[120] = 0;
    line.count = 1; line.order[0] = 0; line_bg[0][120] = 2;
    CHECK(compose(120, 80, &obj, &moved) == 0x7FFF); /* UI background */
    view.bg[0].map = (const uint16_t *)1;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* world background */
    page_shown = 1;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* fading stays behind an open menu */
    obj.colour[120] = 2; obj.prio[120] = 1;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* world sprite behind menu */
    obj.prio[120] = 0;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* untagged priority-zero world sprite */
    obj.flags[120] = 8;
    CHECK(compose(120, 80, &obj, &moved) == 0x7FFF); /* menu sprite stays bright */
    obj.flags[120] = 0;
    obj.colour[120] = 0;
    view.bg[0].map = NULL; view.bg[1].map = (const uint16_t *)1;
    line.count = 2; line.order[0] = 1; line.order[1] = 0;
    line_bg[1][120] = 2;
    line.bldcnt = 1 | (1 << 6) | (0x100 << 1); line.eva = line.evb = 8;
    CHECK(compose(120, 80, &obj, &moved) == 0x3DEF); /* translucent menu over faded world */
    line.bldcnt = 0;
    line_bg[0][120] = 0;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* transparent hole exposes faded world */
    page_shown = 0;
    line.bldcnt = (1 << 1) | (2 << 6); line.evy = 16;
    CHECK(compose(120, 80, &obj, &moved) == 0); /* game's white fade must not revive map edge */
    line.bldcnt = 0;
    view.world = 0;
    prepare_edge_fade();
    CHECK(compose(120, 80, &obj, &moved) == 0x7FFF); /* classic/non-world */
    /* Rasterization must tag the whole HUD sprite, never the upper half of a world sprite. */
    view.world = 1;
    prepare_edge_fade(); line.count = 0; hud_at_edges = 1;
    memset(tiles, 0x11, sizeof tiles); palette[1] = 0x7FFF;
    memset(&obj, 0, sizeof obj);
    sprite(52, entry, oam, tiles, palette, 0, &obj);
    CHECK(obj.colour[120] && !(obj.flags[120] & 8));
    CHECK(compose(120, 52, &obj, &moved) == 0);
    entry[0] = 4;
    sprite(4, entry, oam, tiles, palette, 0, &obj);
    CHECK(obj.colour[0] && (obj.flags[0] & 8));
    CHECK(compose(0, 4, &obj, &moved) == 0x7FFF);
    page_shown = 1; hud_at_edges = 0; entry[0] = 80;
    sprite(80, entry, oam, tiles, palette, 0, &obj);
    CHECK(obj.flags[120] & 8);
    CHECK(compose(120, 80, &obj, &moved) == 0x7FFF);

    view.hud_left = 0; view.hud_right = view.width;
    prepare_edge_fade();
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 0, &obj) == 0);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 479, &obj) == 0);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 2, &obj) == fade(0x7FFF, 9, 0));
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 8, &obj) == 0x7FFF);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 471, &obj) == 0x7FFF);
    view.hud_left = 5;
    prepare_edge_fade();
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 5, &obj) == 0);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 13, &obj) == 0x7FFF);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 479, &obj) == 0);

    view.width = 248; view.shift = 4; view.hud_left = 0; view.hud_right = 248;
    prepare_edge_fade();
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 0, &obj) == 0);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 4, &obj) == 0x7FFF);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 243, &obj) == 0x7FFF);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 247, &obj) == 0);
    view.width = 240; view.shift = 0; view.hud_right = 240;
    prepare_edge_fade();
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 0, &obj) == 0x7FFF);
    CHECK(edge_colour(0x7FFF, LAYER_BACKDROP, 239, &obj) == 0x7FFF);
    return 0;
}
'''


class EdgeFade(unittest.TestCase):
    def test_world_and_player_fade_without_touching_ui(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'edge_fade.c'
            exe = Path(directory) / 'edge_fade.exe'
            source.write_text(HARNESS)
            subprocess.run([build.clang(), '-std=c11', '-I', str(ROOT),
                            '-I', str(ROOT / 'runtime/include'), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
