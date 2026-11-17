package com.shiningsource.android;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

/** API21 keeps ordinary logs/archive; the API30 class is loaded only on supported systems. */
final class AndroidExitInfo {
    static int gamePid(Context context) {
        try {
            ActivityManager manager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
            List<ActivityManager.RunningAppProcessInfo> processes = manager == null ? null : manager.getRunningAppProcesses();
            if (processes == null) return -1; // Cannot prove absence: do not touch a potentially active log.
            for (ActivityManager.RunningAppProcessInfo process : processes)
                if ((context.getPackageName()+":game").equals(process.processName)) return process.pid;
            return 0;
        } catch (RuntimeException unavailable) { return -1; }
    }
    static boolean gameAbsent(Context context) { return gamePid(context) == 0; }
    static void capture(Context context, File external) {
        if (!ExitDiagnostics.supportsHistory(Build.VERSION.SDK_INT) || external == null || !gameAbsent(context)) return;
        try { Api30.capture(context, external); }
        catch (RuntimeException unavailable) { /* OS history is optional; preserve the existing log. */ }
    }
    static void beforePlay(Context context, File external) {
        if (!gameAbsent(context)) return;
        capture(context, external);
        ExitDiagnostics.archive(context.getFilesDir(), external);
    }
    @android.annotation.TargetApi(30)
    private static final class Api30 {
        static void capture(Context context, File external) {
            String startup = DiagnosticReport.tail(DiagnosticReport.statusFile(context.getFilesDir()), DiagnosticReport.STATUS_BYTES);
            ActivityManager manager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
            if (manager == null) return;
            List<android.app.ApplicationExitInfo> history = manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 16);
            List<ExitDiagnostics.Record> records = new ArrayList<>();
            if (history == null) return;
            for (android.app.ApplicationExitInfo item : history)
                records.add(new ExitDiagnostics.Record(item.getProcessName(), item.getPid(), item.getTimestamp(),
                    item.getReason(), item.getStatus(), item.getPss(), item.getRss(), item.getDescription()));
            ExitDiagnostics.Record selected = ExitDiagnostics.select(context.getPackageName()+":game", startup, records);
            ExitDiagnostics.append(context.getFilesDir(), external, startup, selected, !gameAbsent(context));
        }
    }
}
