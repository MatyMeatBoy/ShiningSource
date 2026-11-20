package com.shiningsource.android;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.Os;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * First screen: the user's ROM, the picture and online settings, Play.
 *
 * The ROM is never part of the APK. It reaches the app in one of two ways, and either way it is copied once to
 * rom.gba in the app's own folder (getExternalFilesDir, which is where the game looks for it: host_linux.c), checked,
 * and remembered, so later starts show Play at once:
 *  - "Choose ROM...": the system's file picker;
 *  - found without asking: a .gba file of the supported cartridge in the folders this app may read by itself
 *    (Android/data/PACKAGE/files, Android/media/PACKAGE; on Android 10 and older also Download and the top of
 *    the shared storage, with the storage permission of those versions).
 * The check: a GBA header, the game code at 0xAC and the SHA-1 of the supported dump. A file that is not a GBA
 * ROM, or is another game, is refused and what was stored stays. Another version of this game (a modified dump,
 * another region) is stored with a clear warning: the Windows launcher lets those be tried too.
 *
 * The settings are written as KEY=VALUE lines to shiningsource.env, which the game reads when it starts.
 */
public class MainActivity extends Activity {
    private static final String TAG = "ShiningSource";
    private static final String SHA1 = "aa1288be9257337abdfe3034898cf6c7aa778fbc";      // Shining Soul II (Europe)
    private static final String CODE = "AU2P";                                          // its game code, at 0xAC
    private static final String DEFAULT_SERVER = "45.8.207.150:47900";
    private static final int PICK_ROM = 1, ASK_STORAGE = 2;
    private static final long SMALLEST = 1L << 20, LARGEST = 16L << 20;
    // what a file turned out to be; from OTHER_VERSION on it is kept (the Windows launcher also lets another version be tried)
    private static final int NOT_A_ROM = 0, OTHER_GAME = 1, OTHER_VERSION = 2, MODIFIED = 3, VERIFIED = 4;
    private static final int RED = Color.rgb(242, 115, 102), GREEN = Color.rgb(115, 217, 128), AMBER = Color.rgb(232, 190, 96);

    private boolean spanish, alive;
    private int busy;                           // ROM jobs queued or running
    private String note;                        // what the last ROM job has to say
    private boolean noteIsError;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();     // ROM jobs, one at a time
    private static final ExecutorService REPORT_WORKER = Executors.newSingleThreadExecutor();
    private static final java.util.concurrent.atomic.AtomicBoolean REPORT_SCHEDULED = new java.util.concurrent.atomic.AtomicBoolean();
    private CrashUpload crashUpload;
    private TextView romStatus, romNote, romHelp, playHint, gameNote, gameLanguage;
    private Button choose, play;
    private LauncherTheme theme;
    private CheckBox customServer, directRoom, landscape;
    private EditText room, server, border;
    private RadioButton classic, enhanced;

    /** What a file is: the result of reading it once. */
    private static final class Rom {
        int kind;
        long size;
        String code = "";
    }

    private String text(String english, String other) {
        return spanish ? other : english;
    }

    /** The app's own folder on the shared storage: the game's working folder. Null while the storage is away. */
    private File folder() {
        return getExternalFilesDir(null);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        alive = true;
        spanish = Locale.getDefault().getLanguage().equals("es");
        SharedPreferences saved = getPreferences(MODE_PRIVATE);
        crashUpload = new CrashUpload(getFilesDir());
        crashUpload.setEnabled(saved.getBoolean("automatic_reports", true));
        theme = new LauncherTheme(this);
        int pad = theme.dp(12);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad, pad, pad, pad);
        page.setFocusableInTouchMode(true);         // so that the keyboard does not open by itself
        LinearLayout[] panels = new LinearLayout[4];
        Button[] tabs = new Button[4];
        LinearLayout navigation = new LinearLayout(this);
        navigation.setGravity(Gravity.CENTER);
        navigation.setPadding(pad / 2, pad, pad / 2, 0);
        String[] names = { text("Home", "Inicio"), text("Video", "Vídeo"), text("Online", "En línea"), text("Settings", "Ajustes") };
        for (int i = 0; i < panels.length; i++) {
            final int selected = i;
            LinearLayout panel = new LinearLayout(this);
            panel.setOrientation(LinearLayout.VERTICAL);
            panel.setPadding(pad, pad, pad, pad);
            panel.setBackground(theme.panel());
            panel.setVisibility(i == 0 ? View.VISIBLE : View.GONE);
            panels[i] = panel;
            page.addView(panel);
            Button tab = new Button(this);
            tab.setText(names[i]);
            tabs[i] = tab;
            theme.tab(tab, i, i == 0);
            LinearLayout.LayoutParams tabSize = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
            tabSize.setMargins(pad / 4, 0, pad / 4, 0);
            navigation.addView(tab, tabSize);
            tab.setOnClickListener(view -> {
                for (int j = 0; j < panels.length; j++) {
                    panels[j].setVisibility(j == selected ? View.VISIBLE : View.GONE);
                    theme.tab(tabs[j], j, j == selected);
                }
            });
        }
        LinearLayout home = panels[0], video = panels[1], network = panels[2], settings = panels[3];
        home.addView(theme.hero("ShiningSource", "Shining Soul II"));
        gameLanguage = label("");
        home.addView(gameLanguage);

        romStatus = label("");
        home.addView(romStatus);
        romNote = label("");
        home.addView(romNote);
        romHelp = label("");
        home.addView(romHelp);
        choose = new Button(this);
        choose.setOnClickListener(view -> pick());
        theme.button(choose, false);
        home.addView(choose);

        video.addView(label(text("Display mode", "Modo de imagen")));
        RadioGroup modes = new RadioGroup(this);
        classic = new RadioButton(this);
        classic.setText(text("Classic: the console's picture at a whole scale, inside a border", "Clásico: la imagen de la consola a escala entera, dentro de un borde"));
        theme.choice(classic);
        enhanced = new RadioButton(this);
        enhanced.setText(text("Enhanced: the picture adapts to the screen and shows more of the map", "Mejorado: la imagen se adapta a la pantalla y muestra más mapa"));
        theme.choice(enhanced);
        classic.setId(View.generateViewId());
        enhanced.setId(View.generateViewId());
        modes.addView(classic);
        modes.addView(enhanced);
        String mode = saved.getString("mode", "enhanced");
        modes.check(mode.equals("classic") ? classic.getId() : enhanced.getId());
        modes.setOnCheckedChangeListener((group, id) -> show());
        video.addView(modes);
        border = field(text("Classic border thickness (0–64)", "Grosor del borde clásico (0–64)"), saved.getString("border", "16"), true, video);
        landscape = new CheckBox(this);
        landscape.setText(text("Play in landscape", "Jugar en horizontal"));
        theme.choice(landscape);
        landscape.setChecked(saved.getBoolean("game_landscape", true));
        video.addView(landscape);
        video.addView(label(text("Turn off to follow your device's rotation setting.",
                                 "Desactívalo para respetar el ajuste de rotación del dispositivo.")));

        network.addView(label(text("ShiningSource Server is the default. Create or join from Multiplayer in the game.",
                                   "Servidor ShiningSource es el predeterminado. Crea o únete a una sala desde Multijugador en el juego.")));
        String previousServer = saved.getString("server", DEFAULT_SERVER);
        customServer = new CheckBox(this);
        customServer.setText(text("Connect by IP", "Conectar por IP"));
        theme.choice(customServer);
        customServer.setChecked(saved.getBoolean("custom_server", !previousServer.isEmpty() && !previousServer.equals(DEFAULT_SERVER)));
        network.addView(customServer);
        server = field(text("Relay address (IP or hostname)", "Dirección del servidor (IP o nombre)"), previousServer.equals(DEFAULT_SERVER) ? "" : previousServer, false, network);
        server.setEnabled(customServer.isChecked());
        customServer.setOnCheckedChangeListener((view, checked) -> server.setEnabled(checked));
        directRoom = new CheckBox(this);
        directRoom.setText(text("Use a room from the launcher", "Usar una sala desde el launcher"));
        theme.choice(directRoom);
        directRoom.setChecked(saved.getBoolean("direct_room", false));
        network.addView(directRoom);
        room = field(text("Room", "Sala"), saved.getString("room", ""), false, network);
        room.setEnabled(directRoom.isChecked());
        directRoom.setOnCheckedChangeListener((view, checked) -> room.setEnabled(checked));
        CheckBox automaticReports = new CheckBox(this);
        automaticReports.setText(text("Automatic crash reports", "Informes automáticos de errores"));
        theme.choice(automaticReports);
        automaticReports.setChecked(saved.getBoolean("automatic_reports", true));
        automaticReports.setOnCheckedChangeListener((button, checked) -> {
            getPreferences(MODE_PRIVATE).edit().putBoolean("automatic_reports", checked).commit();
            crashUpload.setEnabled(checked);
            if (checked) { captureCrashReport(); uploadPendingReport(); }
        });
        settings.addView(automaticReports);
        settings.addView(label(text("On return, send device/version and a short error log to ShiningSource's HTTPS service, without ROMs, saves or room/address/path details. Turning this off deletes pending reports.",
            "Al volver, envía dispositivo/versión y un registro breve al servicio HTTPS de ShiningSource, sin ROM, partidas ni detalles de sala/dirección/ruta. Desactivarlo borra los informes pendientes.")));
        if (reportEndpoint().isEmpty()) settings.addView(label(text("Reports stay on this device until the HTTPS report service is configured.",
            "Los informes quedan en este dispositivo hasta configurar el servicio HTTPS.")));

        // Play stays at the bottom of the screen, whatever the length of the page above it
        LinearLayout foot = new LinearLayout(this);
        foot.setOrientation(LinearLayout.VERTICAL);
        foot.setPadding(pad, pad / 2, pad, pad / 2);
        play = new Button(this);
        play.setText(text("PLAY", "JUGAR"));
        play.setTextSize(20);
        play.setOnClickListener(view -> start());
        theme.button(play, true);
        foot.addView(play);
        playHint = label("");
        foot.addView(playHint);
        gameNote = label("");
        gameNote.setTextColor(RED);
        set(gameNote, "");
        foot.addView(gameNote);
        Button report = new Button(this);
        report.setText(text("Share report...", "Compartir informe..."));
        report.setOnClickListener(view -> shareReport());
        theme.button(report, false);
        settings.addView(report);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setBackground(theme.background());
        screen.setFitsSystemWindows(true);          // Android 15 draws under the system bars: keep clear of them
        screen.addView(navigation);
        screen.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        screen.addView(foot, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(screen);
        show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        look(true);             // also after the user went to copy the file and came back
        AndroidExitInfo.capture(this, folder());
        captureCrashReport();
        uploadPendingReport();
        showGameLog();
    }

    @Override
    protected void onPause() {
        super.onPause();
        saveSettings();
    }

    @Override
    protected void onDestroy() {
        alive = false;
        worker.shutdown();
        super.onDestroy();
    }

    private TextView label(String value) {
        TextView view = new TextView(this);
        view.setText(value);
        theme.label(view);
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
        theme.field(edit);
        page.addView(edit);
        return edit;
    }

    private static void set(TextView view, String value) {
        view.setText(value);
        view.setVisibility(value.isEmpty() ? View.GONE : View.VISIBLE);
    }

    /* ---- the stored ROM ---- */

    /** True when rom.gba is the file that was checked and remembered: nothing needs to be read again. */
    private boolean remembered() {
        File folder = folder();
        if (folder == null)
            return false;
        File file = new File(folder, "rom.gba");
        SharedPreferences saved = getPreferences(MODE_PRIVATE);
        return saved.getInt("rom_kind", NOT_A_ROM) >= OTHER_VERSION && file.isFile() && file.length() == saved.getLong("rom_size", -1)
               && file.lastModified() == saved.getLong("rom_time", -1);
    }

    /** A folder as the user sees it in a file manager. */
    private String shown(File folder) {
        String path = folder.getAbsolutePath(), top = Environment.getExternalStorageDirectory().getAbsolutePath();
        return path.startsWith(top + "/") ? path.substring(top.length() + 1) : path;
    }

    /** Puts on screen what is known about the ROM, and whether Play can be pressed. */
    private void show() {
        SharedPreferences saved = getPreferences(MODE_PRIVATE);
        File folder = folder();
        boolean ready = remembered(), chosen = classic.isChecked() || enhanced.isChecked();
        int kind = saved.getInt("rom_kind", NOT_A_ROM);
        String language = folder == null ? "" : gameLanguageName(new File(folder, "shiningsource-language.txt"));
        if (language.isEmpty() && folder != null)
            language = gameLanguageName(new File(folder, "shiningsource-language.txt.bak"));
        gameLanguage.setText(text("Game language", "Idioma del juego") + " " + (language.isEmpty()
            ? text("(to be chosen)", "(por escoger)") : language));

        if (folder == null) {
            romStatus.setText(text("The phone's storage is not available right now. The app keeps the ROM and the saves there.",
                                   "El almacenamiento del teléfono no está disponible ahora. La app guarda allí la ROM y las partidas."));
            romStatus.setTextColor(RED);
        } else if (busy > 0) {
            romStatus.setText(text("Copying and checking the ROM...", "Copiando y comprobando la ROM..."));
            romStatus.setTextColor(AMBER);
        } else if (!ready) {
            romStatus.setText(text("No ROM yet.", "Aún no hay ROM."));
            romStatus.setTextColor(RED);
        } else if (kind == VERIFIED) {
            romStatus.setText(text("ROM ready: Shining Soul II (Europe), verified.", "ROM lista: Shining Soul II (Europa), verificada."));
            romStatus.setTextColor(GREEN);
        } else if (kind == MODIFIED) {
            romStatus.setText(text("ROM stored: it is the European cartridge (" + CODE + "), but its contents differ from the verified dump (SHA-1). The game may not work.",
                                   "ROM guardada: es el cartucho europeo (" + CODE + "), pero su contenido difiere del volcado verificado (SHA-1). Puede que el juego no funcione."));
            romStatus.setTextColor(AMBER);
        } else {
            String code = saved.getString("rom_code", "?");
            romStatus.setText(text("ROM stored, but it is not the supported version: its game code is " + code + " and this port is made for Shining Soul II (Europe), code " + CODE + ". The game will probably not work.",
                                   "ROM guardada, pero no es la versión compatible: su código de juego es " + code + " y este port está hecho para Shining Soul II (Europa), código " + CODE + ". Lo más probable es que el juego no funcione."));
            romStatus.setTextColor(RED);
        }
        set(romNote, note == null ? "" : note);
        romNote.setTextColor(noteIsError ? RED : Color.rgb(200, 206, 220));

        if (folder == null || ready || busy > 0) {
            set(romHelp, "");
        } else {
            StringBuilder places = new StringBuilder();
            for (File place : ownFolders())
                places.append("\n    ").append(shown(place));
            boolean old = Build.VERSION.SDK_INT <= 29;
            if (old)
                places.append("\n    ").append(shown(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)));
            set(romHelp, text("Two ways to add your .gba file:\n"
                              + "1. Press \"Choose ROM...\" and pick it, for example in Download.\n"
                              + "2. Or copy it into one of these folders of the phone's storage (with a file manager, or from a PC over USB) and come back to this screen; it is found by itself:" + places + "\n"
                              + (old ? "" : "On this version of Android an app cannot look into Download by itself: for a file that is there, use the button.\n")
                              + "The file is copied into the app once; afterwards you can delete yours.",
                              "Dos formas de añadir tu archivo .gba:\n"
                              + "1. Pulsa \"Elegir ROM...\" y selecciónalo, por ejemplo en Descargas (Download).\n"
                              + "2. O cópialo a una de estas carpetas del almacenamiento del teléfono (con un gestor de archivos, o desde un PC por USB) y vuelve a esta pantalla; se detecta solo:" + places + "\n"
                              + (old ? "" : "En esta versión de Android una app no puede mirar en Descargas por su cuenta: para un archivo que esté allí, usa el botón.\n")
                              + "El archivo se copia a la app una sola vez; después puedes borrar el tuyo."));
        }
        choose.setText(ready ? text("Change ROM...", "Cambiar ROM...") : text("Choose ROM...", "Elegir ROM..."));
        choose.setEnabled(folder != null && busy == 0);
        play.setEnabled(ready && chosen && busy == 0);
        set(playHint, play.isEnabled() || busy > 0 ? ""
                      : !ready ? text("To play, add your ROM first (above).", "Para jugar, añade primero tu ROM (arriba).")
                               : text("To play, choose above how you want to see the game: Classic or Enhanced.",
                                      "Para jugar, elige arriba cómo quieres ver el juego: Clásico o Mejorado."));
    }

    private static String gameLanguageName(File file) {
        try (InputStream in = new FileInputStream(file)) {
            String code = "" + (char) in.read() + (char) in.read();
            int next = in.read();
            if (next == '\r') {
                if (in.read() != '\n') return "";
                next = in.read();
            } else if (next == '\n') {
                next = in.read();
            }
            if (next != -1) return "";
            switch (code) {
                case "en": return "English";
                case "fr": return "Français";
                case "de": return "Deutsch";
                case "it": return "Italiano";
                case "es": return "Español";
                default: return "";
            }
        } catch (java.io.IOException unavailable) {
            return "";
        }
    }

    /** One ROM job on the worker; the screen is refreshed when it ends. */
    private void job(final Runnable work) {
        busy++;
        show();
        worker.execute(() -> {
            try {
                work.run();
            } catch (Throwable error) {
                Log.w(TAG, "ROM job failed", error);
            }
            runOnUiThread(() -> {
                busy--;
                if (alive)
                    show();
            });
        });
    }

    private void say(String english, String other, boolean error) {
        note = text(english, other);
        noteIsError = error;
        Log.i(TAG, "ROM: " + english);
    }

    /** Reads a stream to its end, copying it to `copy` when there is one, and tells what it is. */
    private static Rom examine(InputStream in, OutputStream copy) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-1");
        byte[] buffer = new byte[1 << 16], header = new byte[0xC0];
        Rom rom = new Rom();
        int got;
        while ((got = in.read(buffer)) != -1) {
            if (rom.size < header.length)
                System.arraycopy(buffer, 0, header, (int) rom.size, (int) Math.min(got, header.length - rom.size));
            rom.size += got;
            if (rom.size > LARGEST)
                return rom;                         // no GBA cartridge is this large: not a ROM
            sha.update(buffer, 0, got);
            if (copy != null)
                copy.write(buffer, 0, got);
        }
        rom.code = code(header);
        if (rom.size < SMALLEST || rom.code == null) {
            rom.code = "";
            return rom;
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : sha.digest())
            hex.append(String.format("%02x", b));
        rom.kind = rom.code.equals(CODE) ? (hex.toString().equals(SHA1) ? VERIFIED : MODIFIED)
                   : rom.code.startsWith(CODE.substring(0, 3)) ? OTHER_VERSION : OTHER_GAME;      // the last letter is the region
        return rom;
    }

    /** The game code of a GBA cartridge header (0xAC), or null when the bytes are not such a header (0xB2 is always 0x96). */
    private static String code(byte[] header) {
        StringBuilder code = new StringBuilder();
        if (header.length < 0xC0 || (header[0xB2] & 0xFF) != 0x96)
            return null;
        for (int i = 0xAC; i < 0xB0; i++)
            code.append(header[i] >= 0x20 && header[i] < 0x7F ? (char) header[i] : '?');
        return code.toString();
    }

    /** Validates a separate temporary copy before atomically replacing the playable ROM. */
    private Rom store(InputStream in, boolean supported) throws Exception {
        File partial = null;
        try (InputStream source = in) {
            File directory = folder();
            if (directory == null)
                throw new java.io.IOException("Game storage unavailable");
            File target = new File(directory, "rom.gba");
            partial = File.createTempFile("rom-", ".part", directory);
            Rom rom;
            try (FileOutputStream out = new FileOutputStream(partial)) {
                rom = examine(source, out);
                if (rom.kind >= (supported ? MODIFIED : OTHER_VERSION))
                    out.getFD().sync();
            }
            if (rom.kind < (supported ? MODIFIED : OTHER_VERSION))
                return rom;
            Os.rename(partial.getAbsolutePath(), target.getAbsolutePath());
            remember(target, rom);
            return rom;
        } finally {
            if (partial != null)
                partial.delete();
        }
    }

    /** Says why a file was not taken. */
    private void refuse(String name, Rom rom) {
        boolean kept = remembered();
        String keptEnglish = kept ? " The ROM you already had is kept." : "", keptOther = kept ? " Se conserva la ROM que ya tenías." : "";
        if (rom.kind == OTHER_GAME) {
            say("\"" + name + "\" is another game (code " + rom.code + "). This port needs your dump of Shining Soul II (Europe), code " + CODE + "." + keptEnglish,
                "\"" + name + "\" es otro juego (código " + rom.code + "). Este port necesita tu volcado de Shining Soul II (Europa), código " + CODE + "." + keptOther, true);
        } else {
            boolean small = rom.size < SMALLEST, large = rom.size > LARGEST;
            say("\"" + name + "\" is not a Game Boy Advance ROM (" + (small ? "it is too small" : large ? "it is too large" : "it has no cartridge header")
                + "). Choose the .gba file of your cartridge." + keptEnglish,
                "\"" + name + "\" no es una ROM de Game Boy Advance (" + (small ? "es demasiado pequeño" : large ? "es demasiado grande" : "no tiene cabecera de cartucho")
                + "). Elige el archivo .gba de tu cartucho." + keptOther, true);
        }
    }

    private void remember(File file, Rom rom) {
        getPreferences(MODE_PRIVATE).edit().putInt("rom_kind", rom.kind).putString("rom_code", rom.code)
                .putLong("rom_size", file.length()).putLong("rom_time", file.lastModified()).commit();
        Log.i(TAG, "ROM: stored " + file + " (" + file.length() + " bytes, code " + rom.code + ", "
                   + (rom.kind == VERIFIED ? "verified" : rom.kind == MODIFIED ? "SHA-1 differs" : "another version") + ")");
    }

    /* ---- found without asking ---- */

    /** The folders on the shared storage that belong to this app: readable without any permission. */
    private List<File> ownFolders() {
        List<File> list = new ArrayList<>();
        try {
            for (File media : getExternalMediaDirs())       // Android/media/PACKAGE: file managers can reach it on every version
                if (media != null)
                    list.add(media);
        } catch (Exception error) {
            // not on this device
        }
        for (File directory : getExternalFilesDirs(null))
            if (directory != null)
                list.add(directory);
        return list;
    }

    /** Where a .gba file is looked for, in order. Folders this app may not read simply list nothing. */
    private List<File> places() {
        List<File> list = ownFolders();
        for (File directory : new ArrayList<>(list)) {
            File[] children = directory.listFiles(File::isDirectory);
            if (children != null)
                for (File child : children)
                    list.add(child);
        }
        File folder = folder();
        if (folder != null && folder.getParentFile() != null)
            list.add(folder.getParentFile());
        try {
            File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            list.add(downloads);                            // where a browser leaves the APK, and the ROM next to it
            File[] inside = downloads.listFiles();
            for (int i = 0; inside != null && i < inside.length; i++)
                if (inside[i].isDirectory())
                    list.add(inside[i]);
            list.add(Environment.getExternalStorageDirectory());
            list.add(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS));
        } catch (Exception error) {
            // the shared storage is away
        }
        return list;
    }

    /** True when the file starts with the header of the supported cartridge. */
    private static boolean looksSupported(File file) {
        byte[] header = new byte[0xC0];
        if (file.length() < SMALLEST || file.length() > LARGEST)
            return false;
        try {
            RandomAccessFile in = new RandomAccessFile(file, "r");
            try {
                in.readFully(header);
            } finally {
                in.close();
            }
            return CODE.equals(code(header));
        } catch (Exception error) {
            return false;
        }
    }

    private boolean mayReadShared() {
        return Build.VERSION.SDK_INT < 23 || Build.VERSION.SDK_INT > 29         // 30 and later: there is nothing to ask for
               || checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** If no ROM is stored and remembered, looks for one. `ask`: may ask for the old storage permission, once. */
    private void look(final boolean ask) {
        final File folder = folder();
        if (folder == null || busy > 0 || remembered()) {
            show();
            return;
        }
        job(() -> {
            File target = new File(folder, "rom.gba");
            try {
                if (target.isFile()) {          // put there by hand, or stored by an earlier version of the app
                    Rom rom;
                    try (InputStream in = new FileInputStream(target)) {
                        rom = examine(in, null);
                    }
                    if (rom.kind >= OTHER_VERSION) {
                        remember(target, rom);
                        return;
                    }
                    refuse("rom.gba", rom);
                }
                SharedPreferences saved = getPreferences(MODE_PRIVATE);
                String savedUri = saved.getString("rom_uri", "");
                if (!savedUri.isEmpty() && restoreRom(Uri.parse(savedUri)))
                    return;
                String savedPath = saved.getString("rom_source_file", "");
                if (!savedPath.isEmpty()) {
                    File original = new File(savedPath);
                    if (!original.equals(target) && looksSupported(original)) {
                        if (store(new FileInputStream(original), true).kind >= MODIFIED)
                            return;
                    }
                }
                for (File place : places()) {
                    File[] files = place.listFiles();
                    Log.i(TAG, "ROM: looking in " + place + (files == null ? " (not readable)" : " (" + files.length + " entries)"));
                    for (int i = 0; files != null && i < files.length; i++) {
                        File file = files[i];
                        if (!file.isFile() || !file.getName().toLowerCase(Locale.ROOT).endsWith(".gba") || file.equals(target) || !looksSupported(file))
                            continue;
                        Log.i(TAG, "ROM: candidate " + file);
                        if (store(new FileInputStream(file), true).kind >= MODIFIED) {
                            forgetDocument(null);
                            saved.edit().putString("rom_source_file", file.getAbsolutePath()).apply();
                            say("Found \"" + file.getName() + "\" in " + shown(place) + " and copied it into the app. You can delete that file if you want.",
                                "Se encontró \"" + file.getName() + "\" en " + shown(place) + " y se copió a la app. Puedes borrar ese archivo si quieres.", false);
                            return;
                        }
                    }
                }
                Log.i(TAG, "ROM: none stored, none found" + (mayReadShared() ? "" : " (the shared storage was not looked at: no permission yet)"));
            } catch (Exception error) {
                Log.w(TAG, "ROM: the search failed", error);
            }
            if (ask && !mayReadShared() && !getPreferences(MODE_PRIVATE).getBoolean("asked_storage", false)) {
                getPreferences(MODE_PRIVATE).edit().putBoolean("asked_storage", true).apply();
                runOnUiThread(() -> {
                    if (alive && Build.VERSION.SDK_INT >= 23)
                        requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE }, ASK_STORAGE);
                });
            }
        });
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == ASK_STORAGE && results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED)
            look(false);
    }

    /* ---- chosen in the file picker ---- */

    private void pick() {
        Intent open = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        open.addCategory(Intent.CATEGORY_OPENABLE);
        open.setType("*/*");                    // .gba has no type of its own
        open.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (Build.VERSION.SDK_INT >= 26) {
            String previous = getPreferences(MODE_PRIVATE).getString("rom_uri", "");
            Uri initial = previous.isEmpty()
                ? DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:" + Environment.DIRECTORY_DOWNLOADS)
                : Uri.parse(previous);
            open.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
        }
        try {
            startActivityForResult(open, PICK_ROM);
            return;
        } catch (Exception error) {
            Log.w(TAG, "no document picker", error);
        }
        try {                                   // devices without the system picker
            Intent get = new Intent(Intent.ACTION_GET_CONTENT);
            get.addCategory(Intent.CATEGORY_OPENABLE);
            get.setType("*/*");
            startActivityForResult(get, PICK_ROM);
        } catch (Exception error) {
            say("This device has no file picker. Copy the .gba file into the app's folder instead.",
                "Este dispositivo no tiene selector de archivos. Copia el archivo .gba a la carpeta de la app.", true);
            show();
        }
    }

    private String nameOf(Uri uri) {
        String name = null;
        try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                name = cursor.getString(0);
        } catch (Exception error) {
            // a provider without names
        }
        if (name == null)
            name = uri.getLastPathSegment();
        return name == null ? "?" : name;
    }

    private Rom readDocument(Uri source) throws Exception {
        InputStream in = getContentResolver().openInputStream(source);
        if (in == null)
            throw new java.io.IOException("Document provider returned no stream");
        return store(in, false);
    }

    private boolean restoreRom(Uri source) {
        try {
            if (readDocument(source).kind >= OTHER_VERSION) {
                say("ROM restored from your previous selection.", "ROM recuperada de tu selección anterior.", false);
                return true;
            }
        } catch (Exception error) {
            Log.i(TAG, "Previously selected ROM is unavailable", error);
        }
        return false;
    }

    private void forgetDocument(Uri replacement) {
        SharedPreferences saved = getPreferences(MODE_PRIVATE);
        String previous = saved.getString("rom_uri", "");
        if (replacement != null && previous.equals(replacement.toString()))
            return;
        saved.edit().remove("rom_uri").apply();
        if (!previous.isEmpty()) {
            try {
                getContentResolver().releasePersistableUriPermission(Uri.parse(previous), Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException unavailable) {
                // The provider already revoked the permission.
            }
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ROM || result != RESULT_OK || data == null || data.getData() == null || folder() == null)
            return;
        final Uri source = data.getData();
        final int grants = data.getFlags();
        job(() -> {
            String name = nameOf(source);
            try {
                Rom rom = readDocument(source);
                if (rom.kind >= OTHER_VERSION) {
                    forgetDocument(source);
                    SharedPreferences.Editor edit = getPreferences(MODE_PRIVATE).edit().remove("rom_source_file");
                    if ((grants & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
                        try {
                            getContentResolver().takePersistableUriPermission(source, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                            edit.putString("rom_uri", source.toString());
                        } catch (SecurityException transientGrant) {
                            // The verified private copy works with transient-only providers too.
                        }
                    }
                    edit.apply();
                    say("\"" + name + "\" was copied into the app.", "\"" + name + "\" se copió a la app.", false);
                } else {
                    refuse(name, rom);
                }
            } catch (Exception error) {
                Log.w(TAG, "ROM: could not read " + source, error);
                say("\"" + name + "\" could not be read or copied.", "No se pudo leer o copiar \"" + name + "\".", true);
            }
        });
    }

    /* ---- Play ---- */

    private void saveSettings() {
        SharedPreferences.Editor edit = getPreferences(MODE_PRIVATE).edit();
        if (classic.isChecked() || enhanced.isChecked())
            edit.putString("mode", classic.isChecked() ? "classic" : "enhanced");
        edit.putString("border", border.getText().toString()).putBoolean("custom_server", customServer.isChecked())
            .putBoolean("direct_room", directRoom.isChecked())
            .putBoolean("game_landscape", landscape.isChecked())
            .putString("room", room.getText().toString())
            .putString("server", server.getText().toString()).apply();
    }

    private void start() {
        File folder = folder();
        if (folder == null || !remembered() || new File(folder, "rom.gba").length() < SMALLEST) {
            look(false);                // the stored file went away or changed: look again instead of starting without it
            return;
        }
        saveSettings();
        try {
            // settings of this run; a file named shiningsource.test.env next to it adds test settings
            PrintWriter env = new PrintWriter(new File(folder, "shiningsource.env"), "UTF-8");
            if (classic.isChecked()) {
                env.println("SS_MODE=classic");
                env.println("SS_BORDER=" + border.getText().toString().trim());
                // the border's look is not set here: the game's options menu chooses it and keeps it (shiningsource.ini)
            }
            String endpoint = customServer.isChecked() ? server.getText().toString().trim() : DEFAULT_SERVER;
            if (endpoint.isEmpty()) {
                env.println("SS_NET_DEFERRED=0");
            } else {
                if (!endpoint.contains(":")) endpoint += ":47900";
                env.println("SS_NET=" + endpoint);
                String roomName = room.getText().toString().trim();
                if (directRoom.isChecked() && !roomName.isEmpty())
                    env.println("SS_NET_ROOM=" + roomName);
                else
                    env.println("SS_NET_DEFERRED=1");
            }
            env.close();
        } catch (Exception error) {
            // the game then starts offline with its defaults
        }
        boolean existingGame = AndroidExitInfo.gamePid(this) > 0;
        AndroidExitInfo.capture(this, folder);
        captureCrashReport();          // Durable outbox before native startup can replace the old log.
        AndroidExitInfo.beforePlay(this, folder);
        if (!existingGame) DiagnosticReport.stage(getFilesDir(), "launch_requested", "");
        set(gameNote, "");
        Log.i(TAG, "starting the game with " + new File(folder, "rom.gba"));
        try {
            startActivity(new Intent(this, GameActivity.class)
                    .putExtra(GameActivity.EXTRA_LANDSCAPE, landscape.isChecked()));
        } catch (RuntimeException failure) {
            DiagnosticReport.stage(getFilesDir(), "startup_error", failure.toString());
            showGameLog();
        }
    }

    private String reportDevice() {
        String version = "unknown";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (PackageManager.NameNotFoundException ignored) { }
        return "App: " + getPackageName() + " " + version
            + "\nDevice: " + Build.MANUFACTURER + " " + Build.MODEL
            + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
            + "\nABIs: " + android.text.TextUtils.join(", ", Build.SUPPORTED_ABIS);
    }

    private String reportEndpoint() {
        int resource = getResources().getIdentifier("crash_report_endpoint", "string", getPackageName());
        return resource == 0 ? "" : getString(resource).trim();
    }

    private void captureCrashReport() {
        if (!getPreferences(MODE_PRIVATE).getBoolean("automatic_reports", true) || !AndroidExitInfo.gameAbsent(this)) return;
        File external = folder();
        File log = external == null ? null : new File(external, "shiningsource.log");
        String startup = DiagnosticReport.tail(DiagnosticReport.statusFile(getFilesDir()), DiagnosticReport.STATUS_BYTES);
        if (DiagnosticReport.shouldQueue(DiagnosticReport.tail(log, DiagnosticReport.LOG_BYTES), startup,
            log == null ? 0 : log.lastModified()))
            crashUpload.capture(DiagnosticReport.createCurrent(reportDevice(), getFilesDir(), external));
    }

    private void uploadPendingReport() {
        String endpoint = reportEndpoint();
        if (endpoint.isEmpty() || !getPreferences(MODE_PRIVATE).getBoolean("automatic_reports", true)
            || !REPORT_SCHEDULED.compareAndSet(false, true)) return;
        final CrashUpload outbox = crashUpload;
        REPORT_WORKER.execute(() -> {
            try { outbox.uploadOne(endpoint, CrashUpload::post); }
            finally { REPORT_SCHEDULED.set(false); }
        });
    }

    private void shareReport() {
        AndroidExitInfo.capture(this, folder());
        captureCrashReport();
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_SUBJECT, "ShiningSource report");
        send.putExtra(Intent.EXTRA_TEXT, DiagnosticReport.create(reportDevice(), getFilesDir(), folder()));
        try { startActivity(Intent.createChooser(send, text("Share report", "Compartir informe"))); }
        catch (RuntimeException unavailable) {
            set(gameNote, text("No app is available to share the report.", "No hay una app para compartir el informe."));
        }
    }

    /** Back from the game: if it could not start or it crashed, say why (the game writes shiningsource.log). */
    private void showGameLog() {
        File folder = folder();
        String startup = DiagnosticReport.tail(DiagnosticReport.statusFile(getFilesDir()), DiagnosticReport.STATUS_BYTES);
        File logFile = folder == null ? null : new File(folder, "shiningsource.log");
        String failure = DiagnosticReport.failure(DiagnosticReport.tail(logFile, DiagnosticReport.LOG_BYTES), startup,
            logFile == null ? 0 : logFile.lastModified());
        if (!failure.isEmpty()) {
            set(gameNote, text("The game stopped with an error:\n", "El juego se detuvo con un error:\n") + failure
                + text("\nUse Share report to see the diagnostic details.", "\nUsa Compartir informe para ver los detalles."));
            Log.w(TAG, "game failure: " + failure);
        } else {
            set(gameNote, "");
        }
    }
}
