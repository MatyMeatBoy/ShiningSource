package com.shiningsource.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/** Plain-Java, bounded diagnostics. Only the two explicit diagnostic files are read. */
final class DiagnosticReport {
    static final int LOG_BYTES = 24000, STATUS_BYTES = 2048, MAX_CHARS = 30000;
    static File statusFile(File internal) { return new File(internal, "startup-status.txt"); }
    static String clip(String text, int limit) {
        if (text == null) return "";
        return text.length() <= limit ? text : text.substring(0, limit - 14) + "\n[truncated]\n";
    }
    static String tail(File file, int limit) {
        if (file == null) return "Unavailable\n";
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            int count = (int)Math.min(in.length(), limit);
            byte[] bytes = new byte[count];
            in.seek(in.length() - count);
            in.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8);
        } catch (Exception unavailable) { return "Unavailable\n"; }
    }
    static void stage(File internal, String stage, String detail) {
        long attempt = System.currentTimeMillis();
        if (!"launch_requested".equals(stage)) {
            long previous = attemptTime(tail(statusFile(internal), STATUS_BYTES));
            if (previous > 0) attempt = previous;
        }
        try (FileOutputStream out = new FileOutputStream(statusFile(internal))) {
            String value = attempt + " " + stage + "\n" + clip(detail, 1000);
            out.write(value.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (Exception unavailable) { /* Diagnostics must never prevent play. */ }
    }
    private static long attemptTime(String startup) {
        try { return Long.parseLong(startup.substring(0, startup.indexOf(' '))); }
        catch (RuntimeException unavailable) { return 0; }
    }
    /** Only explicit terminal markers establish failure. A partial startup log proves neither success nor crash. */
    static String failure(String log, String startup, long logModified) {
        String first = startup.split("\n", 2)[0];
        if (first.endsWith(" startup_error"))
            return clip(startup.substring(Math.min(startup.length(), first.length() + 1)).trim(), 2048);
        if (!hasProcessStage(first)) return "";
        long attempt = attemptTime(startup);
        // Some storage providers round file modification times to whole seconds.
        if (attempt > 0 && logModified < attempt - 1000) return "";
        String failure = "";
        for (String line : log.split("\n")) {
            line = line.trim();
            if (line.startsWith("crash:") || line.startsWith("startup failure:") || line.startsWith("process error:")) failure = line;
            else if (line.startsWith("host64: exit code ")) {
                try {
                    int code = Integer.parseInt(line.substring("host64: exit code ".length()).trim());
                    failure = code == 0 ? "" : line;
                } catch (NumberFormatException incomplete) { /* Wait for a complete marker. */ }
            }
        }
        return clip(failure, 2048);
    }
    private static boolean hasProcessStage(String first) {
        return first.endsWith(" native_enter") || first.endsWith(" native_return")
            || first.endsWith(" loading_libraries") || first.endsWith(" libraries_loaded");
    }
    static boolean shouldQueue(String log, String startup, long logModified) {
        if (!failure(log, startup, logModified).isEmpty()) return true;
        if (!hasProcessStage(startup.split("\n", 2)[0])) return false;
        long attempt = attemptTime(startup);
        if (attempt > 0 && logModified < attempt - 1000) return false;
        java.util.regex.Matcher reason = java.util.regex.Pattern.compile(
            "(?m)^os-exit: Android process exit reason=([0-9]+) .*? status=(-?[0-9]+) ").matcher(log);
        boolean queue = false;
        while (reason.find()) {
            try {
                int code = Integer.parseInt(reason.group(1)), status = Integer.parseInt(reason.group(2));
                queue = code != 10 && code != 11 && !(code == 1 && status == 0);
            } catch (NumberFormatException malformed) { }
        }
        return queue;
    }
    static String createCurrent(String device, File internal, File external) {
        return clip("ShiningSource report\n" + clip(device, 2000)
            + "\n--- Java startup ---\n" + tail(statusFile(internal), STATUS_BYTES)
            + "\n--- Native log (tail) ---\n"
            + tail(external == null ? null : new File(external, "shiningsource.log"), LOG_BYTES), MAX_CHARS);
    }
    static String create(String device, File internal, File external) {
        return clip("ShiningSource report\n" + clip(device, 2000)
            + "\n--- Java startup ---\n" + tail(statusFile(internal), STATUS_BYTES)
            + "\n--- Native log (tail) ---\n"
            + tail(external == null ? null : new File(external, "shiningsource.log"), 17000)
            + "\n--- Previous session (tail) ---\n"
            + tail(external == null ? null : new File(external, "shiningsource.previous.log"), 7000), MAX_CHARS);
    }
}
