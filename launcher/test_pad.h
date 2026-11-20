#pragma once

/* The frame-limited launcher check attaches a process-local pad and sends the
 * same events as a real controller. There is no test device during normal play. */
class LauncherTestPad {
public:
    explicit LauncherTestPad(const char *scenario)
    {
        if (!scenario) return;
        browse = !strcmp(scenario, "browse");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x0079;
        desc.product_id = 0x1844;
        desc.nbuttons = 4;
        desc.name = "Nintendo GameCube (launcher test)";
        device = SDL_AttachVirtualJoystick(&desc);
        if (!device) return;
        char guid[33], mapping[192];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(device), guid, sizeof guid);
        snprintf(mapping, sizeof mapping, "%s,GameCube (launcher test),a:b0,b:b1,dpdown:b2,dpright:b3,", guid);
        SDL_AddGamepadMapping(mapping);
        pad = SDL_OpenGamepad(device);
        if (pad) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, &pad, 1);
    }

    void tick()
    {
        if (!pad) return;
        ++frame;
        SDL_Joystick *stick = SDL_GetGamepadJoystick(pad);
        if (browse) {
            if (frame == 5 || frame == 7) SDL_SetJoystickVirtualButton(stick, 3, frame == 5);
            if (frame == 13 || frame == 15) SDL_SetJoystickVirtualButton(stick, 0, frame == 13);
        } else {
            if (frame == 5 || frame == 7 || frame == 13 || frame == 15)
                SDL_SetJoystickVirtualButton(stick, 0, frame == 5 || frame == 13);
            if (frame == 9 || frame == 11) SDL_SetJoystickVirtualButton(stick, 2, frame == 9);
        }
        SDL_UpdateJoysticks();
    }

    void close()
    {
        if (pad) SDL_CloseGamepad(pad);
        if (device) SDL_DetachVirtualJoystick(device);
    }

private:
    SDL_JoystickID device = 0;
    SDL_Gamepad *pad = nullptr;
    int frame = 0;
    bool browse = false;
};
