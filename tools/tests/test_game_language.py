"""Check the cartridge's language IDs, first-choice persistence and live copy."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build, prepared_source


class GameLanguage(unittest.TestCase):
    def test_sdl_menu_uses_presented_language_without_mapped_cartridge_memory(self):
        host = (ROOT / 'runtime/win32/plat_sdl.c').read_text(encoding='utf-8')
        first = host.index('static enum ss_game_language display_language;')
        header = host[first:host.index('\n}\n', first) + 3]
        first = host.index('enum { ITEM_RESUME,')
        items = host[first:host.index(';', first) + 1]
        first = host.index('static const char *item_label(')
        menu = host[first:host.index('\n}\n', first) + 3]
        first = host.index('    display_language = ', host.index('uint16_t ss_plat_present('))
        receive = host[first:host.index('    update.w = width;', first)]
        harness = r'''
#include "shiningsource/runtime.h"
#include "shiningsource/language.h"
#include <string.h>

static int menu_quit_armed, pad_uses_gamecube;
@UI_TEXT@
@ITEMS@
@MENU@

enum { CANVAS_MAX_H = 2 * SS_SCREEN_H };
static int canvas_w;

static int receive_picture(unsigned packed)
{
    const unsigned bits = packed;
    int width = (int)(bits & 0xFFFFu), height = bits >> 24 ? (int)(bits >> 24) : SS_SCREEN_H;
@RECEIVE@
    (void)height;
    return width;
}

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

int main(void)
{
    CHECK(!strcmp(item_label(ITEM_LANGUAGE), "Game language"));
    CHECK(receive_picture(320 | SS_PRESENT_WIDE_TITLE | SS_PRESENT_HUD |
                          (SS_LANG_ES << SS_PRESENT_LANG_SHIFT)) == 320);
    CHECK(!strcmp(item_label(ITEM_LANGUAGE), "Idioma del juego"));
    CHECK(receive_picture(SS_LANG_IT << SS_PRESENT_LANG_SHIFT) == 0);
    CHECK(!strcmp(item_label(ITEM_LANGUAGE), "Lingua del gioco"));
    CHECK(receive_picture(240 | (7 << SS_PRESENT_LANG_SHIFT)) == 240);
    CHECK(!strcmp(item_label(ITEM_LANGUAGE), "Game language"));
    return 0;
}
'''
        harness = (harness.replace('@UI_TEXT@', header).replace('@ITEMS@', items)
                   .replace('@MENU@', menu).replace('@RECEIVE@', receive))
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / 'host_language.c'
            output = Path(directory) / 'host_language.exe'
            fixture.write_text(harness, encoding='utf-8')
            subprocess.run([build.clang(), '-I', str(ROOT / 'runtime/include'),
                            str(fixture), '-o', str(output)], check=True)
            result = subprocess.run([str(output)], cwd=directory)
            self.assertEqual(result.returncode, 0, f'host language check failed at line {result.returncode}')

    def test_first_choice_is_preserved_and_reused_without_a_save_edit(self):
        source = prepared_source('code_08002118.c')
        harness = r'''
#include <windows.h>
#include "shiningsource/language_file.h"
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

int main(void)
{
    uint32_t *current = (uint32_t *)(uintptr_t)0x03003470u;
    void *page = VirtualAlloc((void *)(uintptr_t)0x03000000u, 0x8000,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(page != NULL);
    CHECK(ss_game_language_preference() == SS_LANG_UNCHOSEN);
    CHECK(ss_game_language_start_menu() < 0);
    CHECK(ss_game_language_current() == SS_LANG_EN);
    CHECK(ss_game_language_preference() == SS_LANG_UNCHOSEN);

    const unsigned cartridge_order[] = {0, 2, 1, 3, 4};
    const char *const copy[] = {"skip", "passer", "abstimmen", "salta", "saltar"};
    for (int slot = 0; slot < SS_LANG_COUNT; slot++) {
        *current = cartridge_order[slot];
        ss_game_language_confirm(*current);
        CHECK(ss_game_language_start_menu() == slot);
        CHECK(!strcmp(ss_game_text("skip", "passer", "abstimmen", "salta", "saltar"), copy[*current]));
    }
    CHECK(ss_game_language_save(SS_LANG_FR));
    CHECK(ss_game_language_current() == SS_LANG_ES);
    CHECK(ss_game_language_start_menu() == 2);
    *current = 999;
    CHECK(ss_game_language_current() == SS_LANG_EN);
    CHECK(!ss_game_language_save(SS_LANG_UNCHOSEN));
    CHECK(ss_game_language_preference() == SS_LANG_FR);
    CHECK(rename(SS_GAME_LANGUAGE_FILE, SS_GAME_LANGUAGE_FILE ".bak") == 0);
    CHECK(ss_game_language_start_menu() == 2);
    CHECK(ss_game_language_save(SS_LANG_IT));
    CHECK(ss_game_language_start_menu() == 3);
    CHECK(ss_game_language_preference_at(SS_GAME_LANGUAGE_FILE ".bak") == SS_LANG_UNCHOSEN);
    FILE *file = fopen(SS_GAME_LANGUAGE_FILE, "wb");
    CHECK(file != NULL);
    fputs("de\r\nunexpected", file);
    CHECK(fclose(file) == 0);
    CHECK(ss_game_language_start_menu() < 0);
    return 0;
}
'''
        self.assertIn('while (preferred_language < 0 && !(1 & pressedMenu000))', source)
        self.assertIn('ss_game_language_confirm((unsigned)gUnk_03003470);', source)
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / 'game_language.c'
            output = Path(directory) / 'game_language.exe'
            fixture.write_text(harness, encoding='ascii')
            subprocess.run([build.clang(), '-I', str(ROOT / 'runtime/include'), str(fixture),
                            str(ROOT / 'runtime/core/language.c'), '-o', str(output)], check=True)
            result = subprocess.run([str(output)], cwd=directory)
            self.assertEqual(result.returncode, 0, f'language check failed at line {result.returncode}')
