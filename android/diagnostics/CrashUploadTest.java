package com.shiningsource.android;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

public final class CrashUploadTest {
    static int checks;
    static void check(boolean condition, String why) { checks++; if (!condition) throw new AssertionError(why); }
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("ss-upload-").toFile();
        try {
            final long[] now = {1000};
            CrashUpload queue = new CrashUpload(root, () -> now[0]);
            String raw = "crash: signal 11 at pc 0x12345678\nroom=PRIVATE_ROOM\npeer 1.2.3.4:1234\nROM /storage/emulated/0/PRIVATE_USER/rom.gba\nSS_NET=PRIVATE_SERVER\n";
            String clean = CrashUpload.sanitize(raw);
            check(!clean.contains("PRIVATE_") && !clean.contains("1.2.3.4"), "redacts network/path data");
            check(clean.contains("crash: signal 11"), "retains useful crash marker");
            check(queue.capture(raw), "durable capture");
            check(!queue.capture(raw), "pending duplicate suppressed");
            check(new CrashUpload(root).pendingCount() == 1, "survives new process instance");
            final int[] calls = {0};
            CrashUpload.Transport offline = (url, id, body) -> { calls[0]++; throw new IOException("offline"); };
            queue.uploadOne("", offline);
            check(calls[0] == 0 && queue.pendingCount() == 1, "empty endpoint never networks");
            queue.uploadOne("https://example.invalid/report", offline);
            check(calls[0] == 1 && queue.pendingCount() == 1, "offline retains pending");
            queue.uploadOne("https://example.invalid/report", offline);
            check(calls[0] == 1, "backoff suppresses immediate retry");
            new CrashUpload(root, () -> now[0]).uploadOne("https://example.invalid/report", offline);
            check(calls[0] == 1, "backoff survives launcher recreation");
            now[0] += 60000;
            queue.uploadOne("https://example.invalid/report", (url, id, body) -> false);
            check(queue.pendingCount() == 1, "rejection retains pending");
            now[0] += 120000;
            queue.uploadOne("https://example.invalid/report", (url, id, body) -> {
                check(id.matches("[0-9a-f]{64}"), "SHA256 identity");
                try {
                    StringBuilder expected = new StringBuilder();
                    for(byte b: java.security.MessageDigest.getInstance("SHA-256").digest(body)) expected.append(String.format("%02x", b & 255));
                    check(id.equals(expected.toString()), "ID matches exact sent UTF8 bytes");
                } catch (java.security.NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
                check(body.length <= CrashUpload.MAX_BYTES, "byte bounded upload");
                check(!new String(body, StandardCharsets.UTF_8).contains("PRIVATE_"), "only redacted body sent");
                return true;
            });
            check(queue.pendingCount() == 0, "accepted report removed");
            check(!new CrashUpload(root).capture(raw), "accepted duplicate suppressed durably");
            File outbox = new File(root,"crash-outbox");
            File accepted = outbox.listFiles((dir,name) -> name.endsWith(".accepted"))[0];
            Files.write(new File(outbox, accepted.getName().replace(".accepted", ".pending")).toPath(), clean.getBytes(StandardCharsets.UTF_8));
            queue.uploadOne("https://example.invalid/report", offline);
            check(calls[0] == 1 && queue.pendingCount() == 0, "accepted marker prevents resend after interrupted pending removal");
            for (int i = 0; i < 20; i++) queue.capture("crash: " + i);
            check(queue.pendingCount() == CrashUpload.MAX_PENDING, "bounded queue");
            queue.setEnabled(false);
            check(queue.pendingCount() == 0 && !queue.capture("crash: disabled"), "opt out purges and disables capture");
            queue.uploadOne("https://example.invalid/report", offline);
            check(calls[0] == 1, "opt out prevents sending");
            queue.setEnabled(true);
            check(queue.capture("crash: reenabled"), "reenable works");
            check(CrashUpload.sanitize(new String(new char[50000]).replace('\0', '\u754c')).getBytes(StandardCharsets.UTF_8).length <= CrashUpload.MAX_BYTES, "unicode bound");
            String ipv6 = CrashUpload.sanitize("server [2001:db8::1]:4321\nroom secret-room\nopen C:\\Users\\Secret User\\file\ncontent://provider/SecretUser\ncrash: Android exit reason=5 status=11 description=/data/Private\n");
            check(!ipv6.contains("2001:") && !ipv6.contains("Secret") && !ipv6.contains("secret-room"), "IPv6, Windows path, document URI redacted");
            check(ipv6.contains("reason=5 status=11"), "OS exit evidence survives private description redaction");
            check(CrashUpload.acceptedStatus(200) && CrashUpload.acceptedStatus(201) && !CrashUpload.acceptedStatus(202)
                && !CrashUpload.acceptedStatus(500), "only persisted/duplicate server acknowledgement is accepted");
            System.out.println("CrashUploadTest: " + checks + " checks passed");
        } finally { erase(root); }
    }
    static void erase(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) erase(child);
        file.delete();
    }
}
