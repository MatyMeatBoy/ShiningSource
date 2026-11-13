package com.shiningsource.android;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.widget.Toast;

import java.util.Locale;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

/**
 * The game: SDL's activity with the host library. Everything else is native (runtime/host64/host_linux.c): it reads
 * rom.gba, the saves and shiningsource.env from the app's own folder, where the first screen put them.
 *
 * Back (the key or the gesture) pressed twice leaves the game and returns to the first screen. SDL only hands the
 * key to the program, which has no use for it, so without this the game could be left through Home alone. Twice,
 * because the gesture is easy to make by accident while playing on the touch controls.
 */
public class GameActivity extends SDLActivity {
    private long backAt;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // a mouse's right button and a controller's own buttons also arrive as Back: those stay with the game
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && (event.getSource() & InputDevice.SOURCE_MOUSE) != InputDevice.SOURCE_MOUSE
            && !SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled())
                back();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private void back() {
        long now = SystemClock.uptimeMillis();
        if (backAt != 0 && now - backAt < 3000) {
            finish();           // SDL then tells the game to quit, and the game's process ends
            return;
        }
        backAt = now;
        boolean spanish = Locale.getDefault().getLanguage().equals("es");
        Toast.makeText(this, spanish ? "Pulsa atrás otra vez para salir del juego" : "Press back again to leave the game", Toast.LENGTH_SHORT).show();
    }
}
