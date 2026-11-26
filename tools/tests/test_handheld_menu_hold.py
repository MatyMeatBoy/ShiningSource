"""The handhelds' menu button: holding Select for two seconds opens the menu, a short press still reaches the game."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]


def handheld_menu_chord():
    """The SS_FB_HOST form of menu_chord in runtime/win32/plat_sdl.c."""
    host = (ROOT / 'runtime/win32/plat_sdl.c').read_text(encoding='utf-8')
    first = host.index('static uint16_t menu_chord(uint16_t pad)\n#ifdef SS_FB_HOST\n')
    body = host[first:host.index('#else', first)]
    return body.replace('#ifdef SS_FB_HOST\n', '')


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
enum { MENU_TOGGLE = 6 };
static int toggles;
static void menu_command(int command) { if (command == MENU_TOGGLE) toggles++; }
@CHORD@
#define CHECK(c) do { if (!(c)) { printf("line %d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    int i, seen = 0;
    /* a short press: hidden while down, then a few pictures of Select for the game, no menu */
    for (i = 0; i < 10; i++) CHECK(menu_chord(0x0004) == 0);
    for (i = 0; i < 4; i++) CHECK(menu_chord(0) == 0x0004);
    CHECK(menu_chord(0) == 0 && toggles == 0);
    /* other buttons pass through untouched while Select is held */
    CHECK(menu_chord(0x0005) == 0x0001);
    while (menu_chord(0)) ;
    /* held two seconds: the menu once, and Select never reaches the game */
    for (i = 0; i < 300; i++) seen |= menu_chord(0x0004);
    CHECK(toggles == 1 && !(seen & 0x0004));
    for (i = 0; i < 10; i++) seen |= menu_chord(0);
    CHECK(!(seen & 0x0004));
    /* the same again closes it */
    for (i = 0; i < 120; i++) menu_chord(0x0004);
    CHECK(toggles == 2);
    return 0;
}
'''


class HandheldMenuHold(unittest.TestCase):
    def test_hold_select_two_seconds(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'chord.c'
            exe = Path(directory) / 'chord.exe'
            source.write_text(HARNESS.replace('@CHORD@', handheld_menu_chord()), encoding='utf-8')
            subprocess.run([build.clang(), str(source), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
