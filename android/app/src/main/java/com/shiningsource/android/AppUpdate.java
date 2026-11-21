package com.shiningsource.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;
import android.widget.Button;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.HashSet;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class AppUpdate {
    private static final String TAG = "ShiningSourceUpdate";
    static final int INSTALL_PERMISSION = 3;
    private final Activity activity;
    private final Button button;
    private final boolean spanish;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private AndroidRelease awaitingPermission;
    private boolean resumed = true;
    private boolean closed;
    private int stagedSession = -1;

    AppUpdate(Activity activity, Button button, boolean spanish) {
        this.activity = activity;
        this.button = button;
        this.spanish = spanish;
    }

    private String text(String english, String other) {
        return spanish ? other : english;
    }

    void check() {
        setBusy(text("Checking for updates…", "Buscando actualizaciones…"));
        worker.execute(() -> {
            try {
                String installed = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
                AndroidRelease release = AndroidRelease.check(activity, installed);
                activity.runOnUiThread(() -> {
                    setReady();
                    if (closed) return;
                    if (release == null) {
                        dialog(text("ShiningSource is up to date.", "ShiningSource está actualizado.")).show();
                    } else {
                        dialog(text("ShiningSource " + release.version + " is available.",
                                "ShiningSource " + release.version + " está disponible."))
                                .setPositiveButton(text("Download update", "Descargar actualización"),
                                        (dialog, which) -> requestInstall(release))
                                .show();
                    }
                });
            } catch (Exception error) {
                showError(error);
            }
        });
    }

    private boolean canInstall() {
        return Build.VERSION.SDK_INT < 26 || activity.getPackageManager().canRequestPackageInstalls();
    }

    private void requestInstall(AndroidRelease release) {
        if (canInstall()) {
            download(release);
            return;
        }
        dialog(text("To install the update, allow ShiningSource to install apps when Android opens Settings.",
                "Para instalar la actualización, permite que ShiningSource instale aplicaciones en Ajustes."))
                .setPositiveButton(text("Open settings", "Abrir Ajustes"), (dialog, which) -> {
                    try {
                        Intent settings = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                Uri.fromParts("package", activity.getPackageName(), null));
                        awaitingPermission = release;
                        activity.startActivityForResult(settings, INSTALL_PERMISSION);
                    } catch (ActivityNotFoundException | SecurityException error) {
                        awaitingPermission = null;
                        showError(error);
                    }
                }).show();
    }

    void permissionResult() {
        AndroidRelease release = awaitingPermission;
        awaitingPermission = null;
        if (release != null && canInstall()) download(release);
    }

    private void download(AndroidRelease release) {
        setBusy(text("Downloading update…", "Descargando actualización…"));
        worker.execute(() -> {
            File apk = null;
            try {
                apk = File.createTempFile("shining-update-", ".apk", activity.getCacheDir());
                try (OutputStream output = new FileOutputStream(apk)) {
                    release.download(activity, output);
                }
                verifyPackage(apk, release.version);
                int id = stage(apk, release.size);
                activity.runOnUiThread(() -> {
                    if (closed) {
                        abandon(id);
                        return;
                    }
                    stagedSession = id;
                    commitWhenResumed();
                });
            } catch (Exception error) {
                showError(error);
            } finally {
                if (apk != null) apk.delete();
            }
        });
    }

    @SuppressWarnings("deprecation")
    private void verifyPackage(File apk, String version) throws IOException, PackageManager.NameNotFoundException {
        PackageManager manager = activity.getPackageManager();
        PackageInfo installed = manager.getPackageInfo(activity.getPackageName(), PackageManager.GET_SIGNATURES);
        PackageInfo candidate = manager.getPackageArchiveInfo(apk.getAbsolutePath(), PackageManager.GET_SIGNATURES);
        if (candidate == null || !installed.packageName.equals(candidate.packageName)
                || !version.equals(candidate.versionName) || versionCode(candidate) <= versionCode(installed)
                || !sameSigners(installed.signatures, candidate.signatures)) {
            throw new IOException("APK package, version or signing certificate does not match");
        }
    }

    @SuppressWarnings("deprecation")
    private static long versionCode(PackageInfo info) {
        return Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
    }

    private static boolean sameSigners(Signature[] installed, Signature[] candidate) {
        return installed != null && candidate != null && installed.length > 0
                && new HashSet<>(Arrays.asList(installed)).equals(new HashSet<>(Arrays.asList(candidate)));
    }

    private PackageInstaller installer() {
        return activity.getPackageManager().getPackageInstaller();
    }

    private int stage(File apk, long length) throws IOException {
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(activity.getPackageName());
        params.setSize(length);
        if (Build.VERSION.SDK_INT >= 31) {
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED);
        }
        int id = installer().createSession(params);
        try (PackageInstaller.Session session = installer().openSession(id);
             FileInputStream input = new FileInputStream(apk);
             OutputStream output = session.openWrite("base.apk", 0, length)) {
            byte[] buffer = new byte[64 * 1024];
            for (int count; (count = input.read(buffer)) != -1; ) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("Update interrupted");
                output.write(buffer, 0, count);
            }
            session.fsync(output);
        } catch (IOException | RuntimeException error) {
            abandon(id);
            throw error;
        }
        return id;
    }

    private void abandon(int id) {
        try {
            installer().abandonSession(id);
        } catch (RuntimeException error) {
            Log.w(TAG, "Could not abandon update session", error);
        }
    }

    private void commitWhenResumed() {
        if (!resumed || stagedSession < 0) return;
        int id = stagedSession;
        stagedSession = -1;
        Intent callback = new Intent(activity, UpdateInstallReceiver.class);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
        try (PackageInstaller.Session session = installer().openSession(id)) {
            PendingIntent status = PendingIntent.getBroadcast(activity, id, callback, flags);
            session.commit(status.getIntentSender());
            setReady();
        } catch (IOException | RuntimeException error) {
            abandon(id);
            showError(error);
        }
    }

    void resume() {
        resumed = true;
        commitWhenResumed();
    }

    void pause() {
        resumed = false;
    }

    void close() {
        closed = true;
        worker.shutdownNow();
        if (stagedSession >= 0) abandon(stagedSession);
    }

    private void setBusy(String label) {
        button.setEnabled(false);
        button.setText(label);
    }

    private void setReady() {
        button.setEnabled(true);
        button.setText(text("Check for updates", "Buscar actualizaciones"));
    }

    private AlertDialog.Builder dialog(String message) {
        return new AlertDialog.Builder(activity)
                .setTitle(text("ShiningSource update", "Actualización de ShiningSource"))
                .setMessage(message)
                .setNeutralButton(text("GitHub releases", "Versiones en GitHub"), (dialog, which) -> openPage())
                .setNegativeButton(android.R.string.cancel, null);
    }

    private void showError(Exception error) {
        Log.w(TAG, "Update failed", error);
        activity.runOnUiThread(() -> {
            if (closed) return;
            setReady();
            dialog(text("Could not complete the update. Try again or open the release page.",
                    "No se pudo completar la actualización. Inténtalo de nuevo o abre la página de versiones.")).show();
        });
    }

    private void openPage() {
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(AndroidRelease.PAGE)));
        } catch (ActivityNotFoundException error) {
            Toast.makeText(activity, text("Could not open the release page.",
                    "No se pudo abrir la página de versiones."), Toast.LENGTH_LONG).show();
        }
    }
}
