package com.shiningsource.android;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.security.MessageDigest;
import java.util.Locale;

/**
 * First screen: the user's ROM, the online settings, Play.
 *
 * The ROM is copied into the app's own folder (it is never part of the APK) and checked against the SHA-1 of the
 * supported version. The settings are written as KEY=VALUE lines to shiningsource.env, which the game reads when
 * it starts.
 */
public class MainActivity extends Activity {
    private static final String SHA1 = "aa1288be9257337abdfe3034898cf6c7aa778fbc";      // Shining Soul II (Europe)
    private static final String DEFAULT_SERVER = "45.8.207.150:47900";
    private static final int PICK_ROM = 1;

    private boolean spanish;
    private TextView romStatus;
    private Button play;
    private CheckBox online;
    private EditText room, delay, server;

    private String text(String english, String other) {
        return spanish ? other : english;
    }

    private File rom() {
        return new File(getExternalFilesDir(null), "rom.gba");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        spanish = Locale.getDefault().getLanguage().equals("es");
        SharedPreferences saved = getPreferences(MODE_PRIVATE);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad, pad, pad, pad);
        page.setBackgroundColor(Color.rgb(18, 23, 36));

        TextView title = new TextView(this);
        title.setText("ShiningSource");
        title.setTextSize(28);
        title.setTextColor(Color.rgb(214, 168, 74));
        page.addView(title);
        page.addView(label(text("Native port of Shining Soul II. The game is not included: choose your own cartridge dump (Europe).",
                                "Port nativo de Shining Soul II. El juego no viene incluido: elige tu propio volcado del cartucho (Europa).")));

        romStatus = label("");
        page.addView(romStatus);
        Button choose = new Button(this);
        choose.setText(text("Choose ROM...", "Elegir ROM..."));
        choose.setOnClickListener(new View.OnClickListener() {
            public void onClick(View view) {
                Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                pick.addCategory(Intent.CATEGORY_OPENABLE);
                pick.setType("*/*");
                startActivityForResult(pick, PICK_ROM);
            }
        });
        page.addView(choose);

        online = new CheckBox(this);
        online.setText(text("Play online (2 to 4 players)", "Jugar en línea (2 a 4 jugadores)"));
        online.setTextColor(Color.WHITE);
        online.setChecked(saved.getBoolean("online", false));
        page.addView(online);
        room = field(text("Room (the same for everyone)", "Sala (la misma para todos)"), saved.getString("room", "shining"), false, page);
        delay = field(text("Delay in frames, the host decides (same country 4, same continent 6, far 10-14)",
                           "Retardo en cuadros, lo decide el anfitrión (mismo país 4, mismo continente 6, lejos 10-14)"),
                      saved.getString("delay", "6"), true, page);
        server = field(text("Server", "Servidor"), saved.getString("server", DEFAULT_SERVER), false, page);
        page.addView(label(text("The first to enter a room is the host. In the game choose MULTIPLAYER MODE.",
                                "El primero en entrar a una sala es el anfitrión. En el juego elige MULTIPLAYER MODE.")));

        play = new Button(this);
        play.setText(text("PLAY", "JUGAR"));
        play.setTextSize(20);
        play.setOnClickListener(new View.OnClickListener() {
            public void onClick(View view) {
                start();
            }
        });
        page.addView(play);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        setContentView(scroll);
        showRom(null);
    }

    private TextView label(String value) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextColor(Color.rgb(200, 206, 220));
        view.setPadding(0, 12, 0, 12);
        return view;
    }

    private EditText field(String caption, String value, boolean number, LinearLayout page) {
        page.addView(label(caption));
        EditText edit = new EditText(this);
        edit.setText(value);
        edit.setSingleLine(true);
        edit.setTextColor(Color.WHITE);
        edit.setGravity(Gravity.START);
        edit.setInputType(number ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        page.addView(edit);
        return edit;
    }

    private void showRom(String digest) {
        File file = rom();
        if (!file.exists()) {
            romStatus.setText(text("No ROM chosen yet.", "Aún no has elegido una ROM."));
            romStatus.setTextColor(Color.rgb(242, 115, 102));
        } else if (digest == null || digest.equals(SHA1)) {
            romStatus.setText(digest == null ? text("ROM ready.", "ROM lista.")
                                             : text("ROM verified: this is the supported version.", "ROM verificada: es la versión compatible."));
            romStatus.setTextColor(Color.rgb(115, 217, 128));
        } else {
            romStatus.setText(text("This file is not the supported version (its SHA-1 differs). The game may not work.",
                                   "Este archivo no es la versión compatible (su SHA-1 es distinto). Puede que el juego no funcione."));
            romStatus.setTextColor(Color.rgb(242, 115, 102));
        }
        play.setEnabled(file.exists());
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ROM || result != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri source = data.getData();
        File target = rom(), partial = new File(target.getPath() + ".part");
        try {
            MessageDigest sha = MessageDigest.getInstance("SHA-1");
            InputStream in = getContentResolver().openInputStream(source);
            OutputStream out = new FileOutputStream(partial);
            byte[] buffer = new byte[65536];
            long total = 0;
            int got;
            while ((got = in.read(buffer)) > 0) {
                total += got;
                if (total > 32L * 1024 * 1024)
                    throw new java.io.IOException("too large");
                sha.update(buffer, 0, got);
                out.write(buffer, 0, got);
            }
            in.close();
            out.close();
            if (!partial.renameTo(target)) {
                target.delete();
                partial.renameTo(target);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : sha.digest())
                hex.append(String.format("%02x", b));
            showRom(hex.toString());
        } catch (Exception error) {
            partial.delete();
            romStatus.setText(text("The file could not be read.", "No se pudo leer el archivo."));
            romStatus.setTextColor(Color.rgb(242, 115, 102));
        }
    }

    private void start() {
        getPreferences(MODE_PRIVATE).edit().putBoolean("online", online.isChecked()).putString("room", room.getText().toString())
                .putString("delay", delay.getText().toString()).putString("server", server.getText().toString()).apply();
        try {
            // settings of this run; a file named shiningsource.test.env next to it adds test settings
            PrintWriter env = new PrintWriter(new File(getExternalFilesDir(null), "shiningsource.env"), "UTF-8");
            if (online.isChecked()) {
                env.println("SS_NET=" + server.getText().toString().trim());
                env.println("SS_NET_ROOM=" + room.getText().toString().trim());
                env.println("SS_NET_DELAY=" + delay.getText().toString().trim());
            }
            env.close();
        } catch (Exception error) {
            // the game then starts offline with its defaults
        }
        startActivity(new Intent(this, GameActivity.class));
    }
}
