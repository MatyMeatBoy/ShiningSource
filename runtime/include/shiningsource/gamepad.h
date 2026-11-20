#ifndef SHININGSOURCE_GAMEPAD_H
#define SHININGSOURCE_GAMEPAD_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>

enum ss_pad_profile { SS_PAD_AUTO, SS_PAD_STANDARD, SS_PAD_GAMECUBE };
enum { SS_PAD_TRIGGER_CLICK = 28672 };

static inline enum ss_pad_profile ss_pad_profile_from_setting(const char *value)
{
    if (value && !strcmp(value, "gamecube")) return SS_PAD_GAMECUBE;
    if (value && !strcmp(value, "standard")) return SS_PAD_STANDARD;
    return SS_PAD_AUTO;
}

static inline const char *ss_pad_profile_name(enum ss_pad_profile profile)
{
    return profile == SS_PAD_GAMECUBE ? "gamecube" : profile == SS_PAD_STANDARD ? "standard" : "auto";
}

static inline void ss_pad_enable_gamecube(void)
{
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1", SDL_HINT_DEFAULT);
}

static inline bool ss_pad_uses_gamecube(SDL_Gamepad *pad, enum ss_pad_profile profile)
{
    if (profile != SS_PAD_AUTO) return profile == SS_PAD_GAMECUBE;
    if (!pad) return false;
    if (SDL_GetRealGamepadType(pad) == SDL_GAMEPAD_TYPE_GAMECUBE) return true;
    const char *name = SDL_GetGamepadName(pad);
    return name && (strstr(name, "GameCube") || strstr(name, "Gamecube") || strstr(name, "gamecube"));
}

static inline SDL_GamepadButton ss_pad_face_button(SDL_Gamepad *pad, SDL_GamepadButtonLabel label, SDL_GamepadButton fallback)
{
    static const SDL_GamepadButton face[] = { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                            SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH };
    for (unsigned i = 0; pad && i < sizeof face / sizeof face[0]; ++i)
        if (SDL_GamepadHasButton(pad, face[i]) && SDL_GetGamepadButtonLabel(pad, face[i]) == label)
            return face[i];
    return fallback;
}

static inline void ss_pad_defaults(SDL_Gamepad *pad, enum ss_pad_profile profile, int *out)
{
    static const int standard[] = {
        SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    };
    memcpy(out, standard, sizeof standard);
    if (ss_pad_uses_gamecube(pad, profile)) {
        out[0] = ss_pad_face_button(pad, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_SOUTH);
        out[1] = ss_pad_face_button(pad, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_WEST);
        out[8] = !pad || SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_MISC4) || SDL_GamepadHasAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                 ? SDL_GAMEPAD_BUTTON_MISC4 : SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        out[9] = !pad || SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_MISC3) || SDL_GamepadHasAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                 ? SDL_GAMEPAD_BUTTON_MISC3 : SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
        if (out[8] != SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && (!pad || SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)))
            out[2] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;   /* Z on the native adapter */
    }
}

static inline bool ss_pad_button_down(SDL_Gamepad *pad, int button, bool gamecube)
{
    SDL_GamepadButton mapped = (SDL_GamepadButton)button;
    if (gamecube && (mapped == SDL_GAMEPAD_BUTTON_MISC3 || mapped == SDL_GAMEPAD_BUTTON_MISC4)
        && !SDL_GamepadHasButton(pad, mapped)) {
        /* Some PC modes expose only analog triggers. Keep a light pull from acting as a digital click. */
        SDL_GamepadAxis axis = mapped == SDL_GAMEPAD_BUTTON_MISC3 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return SDL_GetGamepadAxis(pad, axis) > SS_PAD_TRIGGER_CLICK;
    }
    return SDL_GetGamepadButton(pad, mapped);
}

static inline SDL_GamepadButton ss_pad_capture_trigger(SDL_Gamepad *pad, SDL_GamepadAxis axis, Sint16 value)
{
    SDL_GamepadButton button;
    if (!pad || value <= SS_PAD_TRIGGER_CLICK) return SDL_GAMEPAD_BUTTON_INVALID;
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) button = SDL_GAMEPAD_BUTTON_MISC3;
    else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) button = SDL_GAMEPAD_BUTTON_MISC4;
    else return SDL_GAMEPAD_BUTTON_INVALID;
    return SDL_GamepadHasButton(pad, button) ? SDL_GAMEPAD_BUTTON_INVALID : button;
}

static inline uint16_t ss_pad_stick(SDL_Gamepad *pad, bool gamecube, uint16_t previous)
{
    int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    const int axes[] = { x, -x, -y, y };
    uint16_t held = 0;
    for (int i = 0; i < 4; ++i) {
        uint16_t bit = (uint16_t)(1u << (i + 4));
        /* Leave a gap between press and release so a stick at the edge does not chatter. */
        int threshold = gamecube ? (previous & bit ? 9000 : 12000) : 12000;
        if (axes[i] > threshold) held |= bit;
    }
    return held;
}

#endif
