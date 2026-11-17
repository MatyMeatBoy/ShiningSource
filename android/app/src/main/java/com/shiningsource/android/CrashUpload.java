package com.shiningsource.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;

/** Private, bounded outbox. Network is always called by a worker, outside its file lock. */
final class CrashUpload {
    static final int MAX_BYTES = 32768, MAX_PENDING = 8;
    private static final Object LOCK = new Object();
    private static final java.util.regex.Pattern ABSOLUTE_PATH = java.util.regex.Pattern.compile("(?:^|[\\s=\"'(])/[A-Za-z0-9_]");
    private static boolean sending;
    private final File directory;
    private static volatile boolean enabled = true;
    private final Clock clock;
    interface Clock { long now(); }
    interface Transport { boolean send(String endpoint, String id, byte[] body) throws IOException; }
    CrashUpload(File internal) { this(internal, System::currentTimeMillis); }
    CrashUpload(File internal, Clock clock) { directory = new File(internal, "crash-outbox"); this.clock=clock; }

    static String sanitize(String input) {
        StringBuilder result = new StringBuilder();
        // Do not process unbounded supplied strings; production input is already a bounded report.
        String bounded = input == null ? "" : input.substring(0, Math.min(input.length(), 60000));
        for (String line : bounded.split("\n")) {
            if ((line.startsWith("crash:") || line.startsWith("process error:") || line.startsWith("os-exit:")) && line.contains(" description="))
                line = line.substring(0, line.indexOf(" description="));
            String lower = line.toLowerCase(Locale.ROOT);
            if (lower.matches(".*(room|ss_net|server|peer|relay|udp|netlink).*")) continue;
            if (ABSOLUTE_PATH.matcher(line).find() || lower.matches(".*([a-z]:[\\\\/]|/storage/|/sdcard/|/data/|/home/|/users/|/mnt/|/tmp/|content://|file://).*")) {
                result.append("[private path omitted]\n"); continue;
            }
            line = line.replaceAll("\\b(?:[0-9]{1,3}\\.){3}[0-9]{1,3}(?::[0-9]+)?\\b", "[address]");
            line = line.replaceAll("(?i)(?:[0-9a-f]{0,4}:){2,}[0-9a-f:.%]+", "[address]");
            line = line.replaceFirst("^[0-9]{10,} (native_enter|native_return|startup_error|launch_requested|loading_libraries|libraries_loaded)", "$1");
            result.append(line).append('\n');
        }
        String text = result.toString();
        int end = 0, bytes = 0;
        while (end < text.length()) {
            int code = text.codePointAt(end);
            int count = code <= 0x7f ? 1 : code <= 0x7ff ? 2 : code <= 0xffff ? 3 : 4;
            if (bytes + count > MAX_BYTES) break;
            bytes += count; end += Character.charCount(code);
        }
        return text.substring(0, end);
    }
    private File[] files(String suffix) {
        File[] found = directory.listFiles((dir, name) -> name.matches("[0-9a-f]{64}\\." + suffix));
        return found == null ? new File[0] : found;
    }
    int pendingCount() { synchronized (LOCK) { return files("pending").length; } }
    void setEnabled(boolean value) {
        enabled = value;
        if (!value) synchronized (LOCK) {
            for (File file : files("pending")) file.delete();
            File[] partial = directory.listFiles((dir, name) -> name.matches("[0-9a-f]{64}\\.part"));
            if (partial != null) for (File file : partial) file.delete();
        }
    }
    private long nextAttempt() {
        try (DataInputStream in = new DataInputStream(new FileInputStream(new File(directory,"retry-state")))) {
            return Math.min(in.readLong(), clock.now() + 900000);
        } catch (IOException missing) { return 0; }
    }
    private void retry(boolean failed) {
        File state = new File(directory,"retry-state");
        int failures = 0;
        try (DataInputStream in = new DataInputStream(new FileInputStream(state))) { in.readLong(); failures = in.readInt(); }
        catch (IOException missing) { }
        failures = failed ? Math.min(5, failures + 1) : 0;
        long next = failed ? clock.now() + Math.min(900000, 60000L << (failures - 1)) : 0;
        try (FileOutputStream bytes = new FileOutputStream(state); DataOutputStream out = new DataOutputStream(bytes)) {
            out.writeLong(next); out.writeInt(failures); out.flush(); bytes.getFD().sync();
        } catch (IOException unavailable) { }
    }
    boolean capture(String raw) {
        if (!enabled) return false;
        try {
            byte[] body = sanitize(raw).getBytes(StandardCharsets.UTF_8);
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            StringBuilder id = new StringBuilder();
            for (byte b : sha.digest(body)) id.append(String.format(Locale.ROOT, "%02x", b & 255));
            synchronized (LOCK) {
                if (!enabled || (!directory.isDirectory() && !directory.mkdirs())) return false;
                File[] partials = directory.listFiles((dir, name) -> name.matches("[0-9a-f]{64}\\.part"));
                if (partials != null) for (File partial : partials) partial.delete();
                File pending = new File(directory, id + ".pending"), accepted = new File(directory, id + ".accepted");
                if (pending.exists() || accepted.exists() || files("pending").length >= MAX_PENDING) return false;
                File partial = new File(directory, id + ".part");
                try (FileOutputStream out = new FileOutputStream(partial)) {
                    out.write(body); out.getFD().sync();
                }
                if (!partial.renameTo(pending)) { partial.delete(); return false; }
                return true;
            }
        } catch (Exception unavailable) { return false; }
    }
    void uploadOne(String endpoint, Transport transport) {
        if (!enabled || endpoint == null || !endpoint.startsWith("https://")) return;
        File pending;
        String id;
        byte[] body;
        synchronized (LOCK) {
            if (!enabled || sending || clock.now() < nextAttempt()) return;
            File[] candidates = files("pending");
            if (candidates.length == 0) return;
            Arrays.sort(candidates, (left, right) -> Long.compare(left.lastModified(), right.lastModified()));
            pending = null;
            for (File candidate : candidates) {
                if (new File(directory, candidate.getName().substring(0, 64) + ".accepted").exists()) candidate.delete();
                else { pending = candidate; break; }
            }
            if (pending == null) return;
            id = pending.getName().substring(0, 64);
            if (pending.length() > MAX_BYTES) { pending.delete(); return; }
            try (FileInputStream in = new FileInputStream(pending)) {
                body = new byte[(int)pending.length()];
                int offset = 0, got;
                while (offset < body.length && (got = in.read(body, offset, body.length - offset)) > 0) offset += got;
                if (offset != body.length) return;
            } catch (IOException unavailable) { return; }
            sending = true;
        }
        try {
            if (enabled && transport.send(endpoint, id, body)) synchronized (LOCK) {
                File accepted = new File(directory, id + ".accepted");
                try (FileOutputStream out = new FileOutputStream(accepted)) { out.getFD().sync(); }
                pending.delete();
                retry(false);
                File[] history = files("accepted");
                if (history.length > 128) {
                    Arrays.sort(history, (left, right) -> Long.compare(left.lastModified(), right.lastModified()));
                    for (int i = 0; i < history.length - 128; i++) history[i].delete();
                }
            } else synchronized (LOCK) { if (enabled) retry(true); }
        } catch (IOException offline) { synchronized (LOCK) { if (enabled) retry(true); } }
        finally { synchronized (LOCK) { sending = false; } }
    }
    static boolean post(String endpoint, String id, byte[] body) throws IOException {
        URL url = new URL(endpoint);
        if (!"https".equals(url.getProtocol())) throw new IOException("HTTPS required");
        HttpURLConnection connection = (HttpURLConnection)url.openConnection();
        try {
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(5000); connection.setReadTimeout(5000);
            connection.setRequestMethod("POST"); connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "text/plain; charset=utf-8");
            connection.setRequestProperty("X-Report-ID", id);
            connection.setFixedLengthStreamingMode(body.length);
            try (java.io.OutputStream out = connection.getOutputStream()) { out.write(body); }
            int status = connection.getResponseCode();
            return acceptedStatus(status);
        } finally { connection.disconnect(); }
    }
    static boolean acceptedStatus(int status) { return status == 200 || status == 201; }
}
