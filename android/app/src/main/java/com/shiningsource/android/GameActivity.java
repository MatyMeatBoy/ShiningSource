package com.shiningsource.android;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity with the host library. Everything else is native (runtime/host64/host_linux.c): it reads
 * rom.gba, the saves and shiningsource.env from the app's own folder, where the first screen put them.
 *
 * Back (the key or the gesture) is not handled here: the game asks SDL to hand it over as a key
 * (SDL_ANDROID_TRAP_BACK_BUTTON, set in runtime/win32/plat_sdl.c) and opens its options menu with it, the same
 * menu Escape opens on a PC. Back again closes the menu. Leaving the game is the menu's last entry, which ends
 * the game's process and returns to the first screen; Home leaves it as always.
 */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
