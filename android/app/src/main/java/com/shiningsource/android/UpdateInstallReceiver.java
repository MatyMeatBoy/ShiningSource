package com.shiningsource.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.util.Log;
import android.widget.Toast;

import java.util.Locale;

public final class UpdateInstallReceiver extends BroadcastReceiver {
    @Override
    @SuppressWarnings("deprecation")
    public void onReceive(Context context, Intent result) {
        int status = result.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = result.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                try {
                    confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    context.startActivity(confirm);
                    return;
                } catch (RuntimeException error) {
                    Log.w("ShiningSourceUpdate", "Could not open the installer", error);
                }
            }
        } else if (status == PackageInstaller.STATUS_SUCCESS || status == PackageInstaller.STATUS_FAILURE_ABORTED) {
            return;
        }
        Log.w("ShiningSourceUpdate", "Installer status " + status + ": "
                + result.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        boolean spanish = "es".equals(Locale.getDefault().getLanguage());
        Toast.makeText(context, spanish ? "Android no pudo instalar la actualización."
                : "Android could not install the update.", Toast.LENGTH_LONG).show();
    }
}
