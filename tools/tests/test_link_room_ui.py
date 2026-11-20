"""Drive the in-game Game Link room picker with native GBA button events."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import build


HARNESS = r"""
#include <assert.h>
#include <string.h>
#include <windows.h>

#include "shiningsource/runtime.h"
#include "shiningsource/language.h"

enum { A = 1, B = 2, RIGHT = 16, LEFT = 32, DOWN = 128 };
static const unsigned *input;
static size_t input_count, frame;
static int begins, cancels, polls, pages, stops, width, expected_create, terminal_status;
static uint32_t cursor;
static uint32_t picture[SS_MAX_VIEW_W * SS_SCREEN_H];

void sub_080003F0(void)
{
    assert(frame < input_count);
    assert(ss_link_screen_draw(picture, width));
    assert(picture[3 + 3 * width] == 0xff6c543cu);
    *(uint16_t *)(uintptr_t)0x03001324 = input[frame];
    *(uint16_t *)(uintptr_t)0x03001308 = input[frame++];
}

void ss_net_lobby_page(uint32_t wanted)
{
    cursor = wanted;
    pages++;
}

int ss_net_lobby_list(struct ss_lobby_room rooms[SS_LOBBY_PAGE_SIZE], uint32_t *next)
{
    strcpy(rooms[0].id, cursor ? "ABCDEFGH" : "23456789");
    rooms[0].players = 2;
    *next = cursor ? 0 : 7;
    return 1;
}

void ss_net_lobby_stop(void) { stops++; }
const char *ss_net_lobby_room_id(void) { return NULL; }

int ss_net_lobby_enter(const char *id)
{
    assert((id == NULL) == expected_create);
    if (id)
        assert(strcmp(id, cursor ? "ABCDEFGH" : "23456789") == 0);
    begins++;
    return 1;
}

int ss_net_room_status(void)
{
    return ++polls == 1 ? SS_ROOM_CONNECTING : terminal_status;
}

void ss_net_room_cancel(void)
{
    cancels++;
}

int ss_net_roster_lobby(void) { return 0; }
int ss_net_roster_can_start(void) { return 0; }
int ss_net_roster(struct ss_net_roster_entry seats[4]) { (void)seats; return 0; }

static void check(const unsigned *buttons, size_t count, int create, int expected,
                  int result, int expected_cancels, int expected_pages)
{
    input = buttons;
    input_count = count;
    frame = begins = cancels = polls = pages = stops = 0;
    expected_create = create;
    terminal_status = result;
    assert(ss_link_room_select() == expected);
    assert(begins == 1 && cancels == expected_cancels && polls == 2);
    assert(pages == expected_pages && stops == !expected);
    assert(frame == count);
    assert(!ss_link_screen_draw(picture, width));
}

int main(void)
{
    assert(VirtualAlloc((void *)(uintptr_t)SS_IWRAM, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    unsigned char *rom = VirtualAlloc((void *)(uintptr_t)SS_ROM, 0x1000000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(rom);
    memcpy(rom + 0xac, "AU2P", 4);
    unsigned char *archive = rom + 0x518938;
    memcpy(archive, "SSDT", 4);
    archive[8 + 4 * 4 + 1] = 1; /* Font section at archive + 0x100. */
    archive[0x104] = 8;         /* Font bitmap at section + 8. */

    const unsigned create[] = { 0, A, 0, 0 };
    const unsigned join[] = { DOWN, A, 0, 0 };
    const unsigned paged[] = { RIGHT, DOWN, A, 0, 0 };
    const unsigned missing[] = { DOWN, A, 0, 0, B, B };

    for (width = 240; width <= 427; width += 187) {
        check(create, sizeof create / sizeof *create, 1, 1, SS_ROOM_READY, 0, 1);
        check(join, sizeof join / sizeof *join, 0, 1, SS_ROOM_READY, 0, 1);
        check(paged, sizeof paged / sizeof *paged, 0, 1, SS_ROOM_READY, 0, 2);
        *(uint32_t *)(uintptr_t)0x03003470 = SS_LANG_ES;
        check(missing, sizeof missing / sizeof *missing, 0, 0, SS_ROOM_UNAVAILABLE, 1, 2);
        check(missing, sizeof missing / sizeof *missing, 0, 0, SS_ROOM_ACTIVE, 1, 2);
        *(uint32_t *)(uintptr_t)0x03003470 = SS_LANG_EN;
    }
}
"""


class LinkRoomUiTests(unittest.TestCase):
    def test_create_join_and_return_from_unavailable_room(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, binary = root / "room_menu.c", root / "room_menu.exe"
            harness.write_text(HARNESS, encoding="utf-8")
            subprocess.run([build.clang(), "-std=gnu11", "-Werror",
                            "-I", str(build.ROOT / "runtime/include"),
                            str(harness), str(build.ROOT / "runtime/core/link_lobby.c"),
                            str(build.ROOT / "runtime/core/language.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
