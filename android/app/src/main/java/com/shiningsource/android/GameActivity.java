package com.shiningsource.android;

import org.libsdl.app.SDLActivity;

/** The game: SDL's activity with the host library. Everything else is native (runtime/host64/host_linux.c). */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
