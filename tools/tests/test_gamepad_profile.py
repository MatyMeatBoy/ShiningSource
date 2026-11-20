"""Exercise the shared GameCube profiles against simulated SDL capabilities."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build
import link


HARNESS = r'''
#include <assert.h>
#include "shiningsource/gamepad.h"

struct SDL_Gamepad {
    SDL_GamepadType type;
    const char *name;
    unsigned int buttons, held, axes;
    int stick_x, stick_y, left_trigger, right_trigger;
};
SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad *pad) { return pad->type; }
const char *SDL_GetGamepadName(SDL_Gamepad *pad) { return pad->name; }
bool SDL_GamepadHasButton(SDL_Gamepad *pad, SDL_GamepadButton button) { return !!(pad->buttons & (1u << button)); }
bool SDL_GetGamepadButton(SDL_Gamepad *pad, SDL_GamepadButton button) { return !!(pad->held & (1u << button)); }
bool SDL_GamepadHasAxis(SDL_Gamepad *pad, SDL_GamepadAxis axis) { return !!(pad->axes & (1u << axis)); }
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *pad, SDL_GamepadAxis axis) {
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return (Sint16)pad->stick_x;
    case SDL_GAMEPAD_AXIS_LEFTY: return (Sint16)pad->stick_y;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (Sint16)pad->left_trigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (Sint16)pad->right_trigger;
    default: return 0;
    }
}
SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *pad, SDL_GamepadButton button) {
    if (button == SDL_GAMEPAD_BUTTON_SOUTH) return SDL_GAMEPAD_BUTTON_LABEL_A;
    if (button == (pad->type == SDL_GAMEPAD_TYPE_GAMECUBE ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_EAST))
        return SDL_GAMEPAD_BUTTON_LABEL_B;
    return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
}

#define BUTTON(button) (1u << SDL_GAMEPAD_BUTTON_##button)
#define AXIS(axis) (1u << SDL_GAMEPAD_AXIS_##axis)
int main(void) {
    int bindings[10];
    struct SDL_Gamepad gamecube = {SDL_GAMEPAD_TYPE_GAMECUBE, "Nintendo GameCube",
        BUTTON(SOUTH) | BUTTON(WEST) | BUTTON(RIGHT_SHOULDER) | BUTTON(MISC3) | BUTTON(MISC4),
        0, AXIS(LEFT_TRIGGER) | AXIS(RIGHT_TRIGGER)};
    struct SDL_Gamepad clone = {SDL_GAMEPAD_TYPE_STANDARD, "USB Gamepad",
        BUTTON(SOUTH) | BUTTON(EAST) | BUTTON(BACK), 0, AXIS(LEFT_TRIGGER) | AXIS(RIGHT_TRIGGER)};
    assert(ss_pad_profile_from_setting(NULL) == SS_PAD_AUTO);
    assert(ss_pad_profile_from_setting("invalid") == SS_PAD_AUTO);
    assert(ss_pad_profile_from_setting("gamecube") == SS_PAD_GAMECUBE);
    assert(ss_pad_profile_from_setting("standard") == SS_PAD_STANDARD);
    assert(ss_pad_uses_gamecube(&gamecube, SS_PAD_AUTO));
    assert(!ss_pad_uses_gamecube(&gamecube, SS_PAD_STANDARD));
    assert(!ss_pad_uses_gamecube(&clone, SS_PAD_AUTO));
    assert(ss_pad_uses_gamecube(&clone, SS_PAD_GAMECUBE));
    clone.name = "Mayflash GameCube Controller";
    assert(ss_pad_uses_gamecube(&clone, SS_PAD_AUTO));

    ss_pad_defaults(&gamecube, SS_PAD_AUTO, bindings);
    assert(bindings[0] == SDL_GAMEPAD_BUTTON_SOUTH && bindings[1] == SDL_GAMEPAD_BUTTON_WEST);
    assert(bindings[2] == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && bindings[3] == SDL_GAMEPAD_BUTTON_START);
    assert(bindings[8] == SDL_GAMEPAD_BUTTON_MISC4 && bindings[9] == SDL_GAMEPAD_BUTTON_MISC3);
    gamecube.left_trigger = 32767;
    assert(!ss_pad_button_down(&gamecube, bindings[9], true));   /* analog does not preempt a digital click */
    assert(ss_pad_capture_trigger(&gamecube, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767) == SDL_GAMEPAD_BUTTON_INVALID);
    gamecube.held = BUTTON(MISC3);
    assert(ss_pad_button_down(&gamecube, bindings[9], true));
    assert(!ss_pad_button_down(&gamecube, bindings[8], true));

    ss_pad_defaults(&clone, SS_PAD_GAMECUBE, bindings);
    assert(bindings[0] == SDL_GAMEPAD_BUTTON_SOUTH && bindings[1] == SDL_GAMEPAD_BUTTON_EAST);
    assert(bindings[2] == SDL_GAMEPAD_BUTTON_BACK);
    clone.left_trigger = 16000;
    assert(!ss_pad_button_down(&clone, bindings[9], true));
    clone.left_trigger = 30000;
    assert(ss_pad_button_down(&clone, bindings[9], true));
    assert(!ss_pad_button_down(&clone, bindings[9], false));
    assert(ss_pad_capture_trigger(&clone, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 30000) == SDL_GAMEPAD_BUTTON_MISC3);
    assert(ss_pad_capture_trigger(&clone, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1000) == SDL_GAMEPAD_BUTTON_INVALID);
    assert(ss_pad_capture_trigger(&clone, SDL_GAMEPAD_AXIS_LEFTX, 32767) == SDL_GAMEPAD_BUTTON_INVALID);

    ss_pad_defaults(&clone, SS_PAD_STANDARD, bindings);
    assert(bindings[0] == SDL_GAMEPAD_BUTTON_EAST && bindings[1] == SDL_GAMEPAD_BUTTON_SOUTH);
    assert(bindings[8] == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && bindings[9] == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    clone.stick_x = 11000;
    assert(ss_pad_stick(&clone, true, 0) == 0);
    clone.stick_x = 15000;
    clone.stick_y = -16000;
    assert(ss_pad_stick(&clone, true, 0) == 0x50);
    clone.stick_x = 10000;
    clone.stick_y = -10000;
    assert(ss_pad_stick(&clone, true, 0x50) == 0x50);
    assert(ss_pad_stick(&clone, false, 0x50) == 0);
    clone.stick_x = 0;
    clone.stick_y = 0;
    assert(ss_pad_stick(&clone, true, 0x50) == 0);
    return 0;
}
'''


class GamepadProfile(unittest.TestCase):
    def test_controller_layouts_and_input(self):
        sdl = next((Path(h) / 'x86_64-w64-mingw32' for h in link.SDL_HINTS
                    if h and (Path(h) / 'x86_64-w64-mingw32/include/SDL3').is_dir()), None)
        self.assertIsNotNone(sdl, 'set SHININGSOURCE_SDL3 to the SDL SDK')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'gamepad.c'
            exe = source.with_suffix('.exe')
            source.write_text(HARNESS, encoding='utf-8')
            subprocess.run([build.clang(), '-std=c11', '-I', str(sdl / 'include'),
                            '-I', str(ROOT / 'runtime/include'), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
