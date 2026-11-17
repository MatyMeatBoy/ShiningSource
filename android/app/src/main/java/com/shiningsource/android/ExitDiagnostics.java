package com.shiningsource.android;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Plain-Java selection and bounded local persistence; no network or tombstone contents. */
final class ExitDiagnostics {
    static final int ARCHIVE_BYTES = 28000;
    static boolean supportsHistory(int sdk) { return sdk >= 30; }
    static final class Record {
        final String process, description;
        final int pid, reason, status;
        final long timestamp, pss, rss;
        Record(String process, int pid, long timestamp, int reason, int status, long pss, long rss, String description) {
            this.process=process; this.pid=pid; this.timestamp=timestamp; this.reason=reason; this.status=status;
            this.pss=pss; this.rss=rss; this.description=description;
        }
    }
    static long attemptTime(String startup) {
        try { return Long.parseLong(startup.substring(0, startup.indexOf(' '))); }
        catch (RuntimeException unavailable) { return 0; }
    }
    static int attemptPid(String startup) {
        Matcher match = Pattern.compile("(?m)^pid=([0-9]+)$").matcher(startup);
        try { return match.find() ? Integer.parseInt(match.group(1)) : 0; }
        catch (NumberFormatException unavailable) { return 0; }
    }
    static Record select(String process, String startup, List<Record> records) {
        long attempt = attemptTime(startup);
        if (attempt <= 0) return null;
        int pid = attemptPid(startup);
        Record selected = null;
        for (Record item : records) {
            if (!process.equals(item.process) || item.timestamp < attempt || (pid != 0 && item.pid != pid)) continue;
            if (selected == null || item.timestamp > selected.timestamp) selected = item;
        }
        return selected;
    }
    private static String reasonName(int reason) {
        String[] names = {"unknown", "exit-self", "signaled", "low-memory", "java-crash", "native-crash", "anr",
            "initialization-failure", "permission-change", "excessive-resource-usage", "user-requested",
            "user-stopped", "dependency-died", "other", "freezer"};
        return reason >= 0 && reason < names.length ? names[reason] : "other";
    }
    static String line(Record item) {
        String prefix = item.reason == 4 || item.reason == 5 || item.reason == 6 ? "crash:"
            : item.reason == 7 || (item.reason == 1 && item.status != 0) ? "process error:" : "os-exit:";
        String description = item.description == null ? "" : item.description.replace('\n',' ').replace('\r',' ');
        return prefix + " Android process exit reason=" + item.reason + " (" + reasonName(item.reason) + ")"
            + " status=" + item.status + " pid=" + item.pid + " timestamp=" + item.timestamp
            + " pss_kib=" + item.pss + " rss_kib=" + item.rss + " description=" + DiagnosticReport.clip(description, 600);
    }
    static synchronized boolean append(File internal, File external, String expectedStartup, Record item, boolean gameAlive) {
        if (gameAlive || external == null || item == null) return false;
        if (!expectedStartup.equals(DiagnosticReport.tail(DiagnosticReport.statusFile(internal), DiagnosticReport.STATUS_BYTES))) return false;
        long attempt = attemptTime(expectedStartup);
        if (attempt <= 0 || item.timestamp < attempt || (attemptPid(expectedStartup) != 0 && attemptPid(expectedStartup) != item.pid)) return false;
        String id = attempt + ":" + item.pid + ":" + item.timestamp + ":" + item.reason + ":" + item.status;
        File receipt = new File(internal,"last-exit-info.txt"), log = new File(external,"shiningsource.log");
        if (log.exists() && log.lastModified() < attempt - 1000) return false;
        String marker = "exit-info-id=" + id;
        if (DiagnosticReport.tail(receipt, 256).trim().equals(id) || DiagnosticReport.tail(log, 24000).contains(marker)) return false;
        try {
            byte[] text = ("\n" + line(item) + "\n" + marker + "\n").getBytes(StandardCharsets.UTF_8);
            try (FileOutputStream out = new FileOutputStream(log,true)) { out.write(text); out.getFD().sync(); }
            try (FileOutputStream out = new FileOutputStream(receipt)) { out.write(id.getBytes(StandardCharsets.UTF_8)); out.getFD().sync(); }
            return true;
        } catch (Exception unavailable) { return false; }
    }
    static void archive(File internal, File external) {
        if (external == null) return;
        File log = new File(external,"shiningsource.log");
        if (!log.isFile()) return;
        String text = "Previous session (bounded tail)\n--- Java startup ---\n"
            + DiagnosticReport.tail(DiagnosticReport.statusFile(internal), DiagnosticReport.STATUS_BYTES)
            + "\n--- Native log ---\n" + DiagnosticReport.tail(log, DiagnosticReport.LOG_BYTES);
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        try (FileOutputStream out = new FileOutputStream(new File(external,"shiningsource.previous.log"))) {
            out.write(bytes, Math.max(0, bytes.length - ARCHIVE_BYTES), Math.min(bytes.length, ARCHIVE_BYTES)); out.getFD().sync();
        } catch (Exception unavailable) { /* An archive failure must not prevent play. */ }
    }
}
