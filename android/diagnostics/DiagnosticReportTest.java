package com.shiningsource.android;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** Run with javac/java; no Android, ROM or emulator required. */
public final class DiagnosticReportTest {
    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("ss-diagnostics-").toFile();
        File internal = new File(root, "internal"), external = new File(root, "external");
        internal.mkdir(); external.mkdir();
        try {
            DiagnosticReport.stage(internal, "startup_error", "UnsatisfiedLinkError: test");
            check(DiagnosticReport.tail(DiagnosticReport.statusFile(internal), 2048).contains("startup_error\n"), "durable startup failure");
            byte[] log = new byte[90000]; Arrays.fill(log, (byte)'x');
            byte[] end = "RECENT_NATIVE_FAILURE".getBytes(StandardCharsets.UTF_8);
            System.arraycopy(end, 0, log, log.length - end.length, end.length);
            Files.write(new File(external, "shiningsource.log").toPath(), log);
            Files.write(new File(external, "rom.gba").toPath(), "PRIVATE_ROM_SENTINEL".getBytes(StandardCharsets.UTF_8));
            Files.write(new File(external, "shiningsource.env").toPath(), "PRIVATE_SETTINGS_SENTINEL".getBytes(StandardCharsets.UTF_8));
            String report = DiagnosticReport.create("Device: test API 35 ABI arm64-v8a", internal, external);
            check(report.length() <= DiagnosticReport.MAX_CHARS, "share bound");
            check(report.contains("RECENT_NATIVE_FAILURE"), "reads recent tail");
            check(!report.contains("PRIVATE_"), "does not read ROM/settings");
            check(report.contains("UnsatisfiedLinkError"), "includes Java failure");
            check(DiagnosticReport.tail(new File(external, "shiningsource.log"), 100).length() == 100, "read byte bound");
            check(DiagnosticReport.create("device", internal, null).contains("Unavailable"), "external storage unavailable");
            check(DiagnosticReport.tail(new File(root, "missing"), 100).contains("Unavailable"), "missing file");
            DiagnosticReport.stage(internal, "launch_requested", "");
            check(!DiagnosticReport.tail(DiagnosticReport.statusFile(internal), 2048).contains("startup_error"), "new attempt clears old failure");
            String attempt = DiagnosticReport.tail(DiagnosticReport.statusFile(internal), 2048).split(" ", 2)[0];
            DiagnosticReport.stage(internal, "native_return", "");
            check(DiagnosticReport.tail(DiagnosticReport.statusFile(internal), 2048).startsWith(attempt + " native_return"), "native return retains attempt start time");
            String started = "10000 native_enter\n";
            check(DiagnosticReport.failure("host64: image loaded\nhost64: exit code 0\n", started, 11000).isEmpty(), "normal exit");
            check(DiagnosticReport.failure("host64: image loaded\n", started, 11000).isEmpty(), "started is not a crash");
            check(DiagnosticReport.failure("device: model\n", started, 11000).isEmpty(), "incomplete startup is not a crash");
            check(DiagnosticReport.failure("crash: signal 11\n", started, 11000).contains("signal 11"), "explicit crash");
            check(DiagnosticReport.failure("image: unsupported format\nstartup failure: image rejected\n", started, 11000).contains("image rejected"), "startup failure reason");
            check(DiagnosticReport.failure("host64: exit code 4\n", started, 11000).contains("exit code 4"), "nonzero exit");
            check(DiagnosticReport.failure("crash: old run\n", "10000 launch_requested\n", 11000).isEmpty(), "pre-native launch ignores old log");
            check(DiagnosticReport.failure("crash: old run\n", started, 1000).isEmpty(), "stale log ignored");
            check(DiagnosticReport.failure("host64: exit code 0\n", "10000 startup_error\nUnsatisfiedLinkError", 11000).contains("UnsatisfiedLinkError"), "Java failure overrides stale normal log");
            check(DiagnosticReport.failure("crash: signal 11\nhost64: exit code 0\n", started, 11000).isEmpty(), "latest terminal marker wins");
            System.out.println("DiagnosticReportTest: 20 checks passed");
        } finally {
            for (File dir : new File[]{internal, external}) {
                for (File file : dir.listFiles()) file.delete();
                dir.delete();
            }
            root.delete();
        }
    }
}
