"""Desktop window settings remain compatible with pre-0.9.1 installations."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]


class WindowMode(unittest.TestCase):
    def test_new_defaults_legacy_and_precedence(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'window_mode.c'
            binary = Path(temp) / 'window_mode.exe'
            source.write_text('''
#include <assert.h>
#include "shiningsource/window_mode.h"

int main(void) {
    assert(ss_window_mode_from_settings(NULL, NULL) == SS_WINDOW_BORDERLESS);
    assert(ss_window_mode_from_settings("", "") == SS_WINDOW_BORDERLESS);
    assert(ss_window_mode_from_settings(NULL, "0") == SS_WINDOW_WINDOWED);
    assert(ss_window_mode_from_settings(NULL, "1") == SS_WINDOW_BORDERLESS);
    assert(ss_window_mode_from_settings("unknown", "0") == SS_WINDOW_WINDOWED);
    assert(ss_window_mode_from_settings("windowed", "1") == SS_WINDOW_WINDOWED);
    assert(ss_window_mode_from_settings("borderless", "0") == SS_WINDOW_BORDERLESS);
    assert(ss_window_mode_from_settings("fullscreen", "0") == SS_WINDOW_FULLSCREEN);
    for (int mode = SS_WINDOW_WINDOWED; mode < SS_WINDOW_MODE_COUNT; ++mode)
        assert(ss_window_mode_from_settings(ss_window_mode_name(mode), NULL) == mode);
    return 0;
}
''', encoding='utf-8')
            subprocess.run([build.clang(), '-std=c11', '-Werror', '-I',
                            str(ROOT / 'runtime/include'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_recomplauncher_persists_all_three_modes(self):
        profile = json.loads((ROOT / 'recomplauncher.json').read_text(encoding='utf-8'))
        modes = next((setting for setting in profile['settings'] if setting['id'] == 'window_mode'), None)
        self.assertIsNotNone(modes)
        self.assertEqual(modes['defaults'], {'window_mode': 'borderless'})
        self.assertEqual([choice['set'] for choice in modes['choices']], [
            {'window_mode': 'windowed'}, {'window_mode': 'borderless'}, {'window_mode': 'fullscreen'},
        ])

    def test_only_active_frontend_tasks_annotate_wide_frames(self):
        scene = (ROOT / 'runtime/core/scene.c').read_text(encoding='utf-8')
        first = scene.index('int ss_frontend_picture(void)')
        classifier = scene[first:scene.index('\n}\n', first) + 3]
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'frontend.c'
            binary = Path(temp) / 'frontend.exe'
            source.write_text('''
#include <windows.h>
#include <assert.h>
#include "shiningsource/runtime.h"

void sub_0800232C(void) {}
void sub_08002694(void) {}
void sub_08002B70(void) {}
void sub_08003984(void) {}
void sub_080904A4(void) {}
void sub_08086860(void) {}
void sub_0807A5D4(void) {}
void sub_0807B7B4(void) {}
void sub_08082BB0(void) {}
void sub_080855D4(void) {}
''' + classifier + '''

static uint32_t *task(unsigned index) { return (uint32_t *)(0x03001300 + (uintptr_t)index * 0x28); }
static void link(unsigned index, void (*entry)(void), uint32_t signal) {
    uint32_t *slot = task(index);
    slot[1] = 4;
    slot[3] = (uint32_t)(uintptr_t)entry;
    slot[5] = signal;
    if (index > 0) task(index - 1)[0] = (uint32_t)(uintptr_t)slot;
}

int main(void) {
    assert(VirtualAlloc((void *)(uintptr_t)SS_EWRAM, 0x40000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(VirtualAlloc((void *)(uintptr_t)SS_IWRAM, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(VirtualAlloc((void *)(uintptr_t)SS_IO, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint16_t *io = (uint16_t *)(uintptr_t)SS_IO;
    *(uint32_t *)(uintptr_t)0x03001240 = (uint32_t)(uintptr_t)task(0);
    link(0, sub_08002694, 0x14); /* Persistent title task sleeps during gameplay. */
    io[0] = 0x1240;
    io[5] = 0x1D00;
    assert(ss_frontend_picture() == 0);
    link(0, sub_08002694, 1);
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_TITLE);
    io[5] = 0x1D01;
    assert(ss_frontend_picture() == 0); /* Another screen owns the hardware layer. */
    link(1, sub_0800232C, 1);
    io[0] = 0x0240;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_BRAND);
    io[0] = 0x1240;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_LANGUAGE);
    link(1, sub_08002B70, 0x5A112EBF); /* Suspended during character setup. */
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_SAVE);
    link(2, sub_08003984, 1);
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_FOREST);
    link(3, sub_08086860, 1);
    assert(ss_frontend_picture() == 0); /* Story/gameplay takes over. */
    task(2)[0] = 0;
    link(2, sub_080904A4, 1);
    io[0] = 0x1100;
    io[4] = 0x1801;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_SOUND);
    io[4] = 0x1c08;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_SAVE); /* Do not use the sound-test backdrop before it owns BG0. */
    task(1)[0] = 0;
    link(0, sub_08002694, 0x14);
    uint32_t *menu = (uint32_t *)(uintptr_t)0x03006450;
    uint32_t *manager_slot = (uint32_t *)(uintptr_t)0x03006440;
    menu[1] = 4;
    menu[3] = (uint32_t)(uintptr_t)sub_0807A5D4;
    task(0)[0] = 0x0A1BBCB0; /* A separate native task can precede the menu on the scheduler list. */
    io[7] = 0x1F03;
    io[0] = 0x2040;
    assert(ss_frontend_picture() == 0); /* No allocated manager. */
    *manager_slot = SS_EWRAM + 0x20000;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_TRANSITION);
    io[0] = 0x1F40;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_MARBLE);
    menu[3] = (uint32_t)(uintptr_t)sub_0807B7B4;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_MARBLE);
    menu[3] = (uint32_t)(uintptr_t)sub_08082BB0;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_SHOP);
    io[0] = 0x1d40;
    io[5] = 0x1d03;
    io[6] = 0x9a01;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_SHOP); /* The same BG layout can still show the ordinary lists. */
    *(uint32_t *)(uintptr_t)(*manager_slot + 0x28) = 1;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_SHOP_INSPECT);
    menu[3] = (uint32_t)(uintptr_t)sub_0807B7B4;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_INSPECT);
    io[0] |= 0x0080;
    assert(ss_frontend_picture() == 0); /* Forced blank applies to the entire display. */
    io[0] = 0x1f40;
    io[7] = 0x1E03;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_TRANSITION); /* BG3 owned by old field. */
    menu[1] = 0;
    assert(ss_frontend_picture() == 0); /* Retained callback after teardown. */
    menu[1] = 4;
    menu[3] = (uint32_t)(uintptr_t)sub_080855D4;
    io[7] = 0x1e00;
    assert(ss_frontend_picture() == SS_PRESENT_WIDE_CATALOG);
    assert((SS_PRESENT_WIDE_MASK & (SS_PRESENT_LANG_MASK | SS_PRESENT_HUD | 0xffff)) == 0);
    return 0;
}
''', encoding='utf-8')
            subprocess.run([build.clang(), '-std=c11', '-Werror', '-I',
                            str(ROOT / 'runtime/include'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
