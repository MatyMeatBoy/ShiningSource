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
    public void loadLibraries() {
        DiagnosticReport.stage(getFilesDir(), "loading_libraries", "");
        try {
            super.loadLibraries();
            DiagnosticReport.stage(getFilesDir(), "libraries_loaded", "");
        } catch (RuntimeException | Error failure) {
            DiagnosticReport.stage(getFilesDir(), "startup_error", failure.toString());
            throw failure; // SDL retains its normal error dialog and cleanup.
        }
    }

    @Override
    protected void main() {
        DiagnosticReport.stage(getFilesDir(), "native_enter", "");
        try {
            super.main();
            DiagnosticReport.stage(getFilesDir(), "native_return", "");
        } catch (RuntimeException | Error failure) {
            DiagnosticReport.stage(getFilesDir(), "startup_error", failure.toString());
            throw failure;
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
