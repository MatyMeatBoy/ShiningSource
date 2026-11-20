package com.shiningsource.android;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;

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
    static final String EXTRA_LANDSCAPE = "com.shiningsource.android.GAME_LANDSCAPE";

    private volatile boolean landscape = true;

    @Override
    protected void onCreate(Bundle state) {
        landscape = getIntent().getBooleanExtra(EXTRA_LANDSCAPE, true);
        applyOrientation();
        super.onCreate(state);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        landscape = intent.getBooleanExtra(EXTRA_LANDSCAPE, true);
        applyOrientation();
    }

    @Override
    public void setOrientationBis(int width, int height, boolean resizable, String hint) {
        // SDL requests orientation again when it creates or resizes the game window.
        if (width > 1 && height > 1)
            applyOrientation();
    }

    private void applyOrientation() {
        int policy = landscape ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                               : ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
        if (getRequestedOrientation() != policy)
            setRequestedOrientation(policy);
    }

    @Override
    public void loadLibraries() {
        DiagnosticReport.stage(getFilesDir(), "loading_libraries", "pid=" + android.os.Process.myPid());
        try {
            super.loadLibraries();
            DiagnosticReport.stage(getFilesDir(), "libraries_loaded", "pid=" + android.os.Process.myPid());
        } catch (RuntimeException | Error failure) {
            DiagnosticReport.stage(getFilesDir(), "startup_error", "pid=" + android.os.Process.myPid() + "\n" + failure.toString());
            throw failure; // SDL retains its normal error dialog and cleanup.
        }
    }

    @Override
    protected void main() {
        DiagnosticReport.stage(getFilesDir(), "native_enter", "pid=" + android.os.Process.myPid());
        try {
            super.main();
            DiagnosticReport.stage(getFilesDir(), "native_return", "pid=" + android.os.Process.myPid());
        } catch (RuntimeException | Error failure) {
            DiagnosticReport.stage(getFilesDir(), "startup_error", "pid=" + android.os.Process.myPid() + "\n" + failure.toString());
            throw failure;
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
