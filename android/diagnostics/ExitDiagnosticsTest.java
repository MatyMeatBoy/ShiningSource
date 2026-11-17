package com.shiningsource.android;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class ExitDiagnosticsTest {
    static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    static ExitDiagnostics.Record record(String process, int pid, long when, int reason, int status) {
        return new ExitDiagnostics.Record(process, pid, when, reason, status, 123, 456, "description");
    }
    public static void main(String[] args) throws Exception {
        String attempt = "10000 native_enter\npid=77\n";
        check(!ExitDiagnostics.supportsHistory(21) && !ExitDiagnostics.supportsHistory(29), "API21-29 fallback never queries exit history");
        check(ExitDiagnostics.supportsHistory(30), "API30 history supported");
        ExitDiagnostics.Record match = record("pkg:game", 77, 11000, 5, 11);
        check(ExitDiagnostics.select("pkg:game", attempt, Arrays.asList(record("pkg",77,11000,5,11), record("pkg:game",78,12000,5,11), record("pkg:game",77,9000,5,11),match)) == match, "process PID timestamp filter");
        check(ExitDiagnostics.select("pkg:game", "12000 native_enter\npid=77\n", Arrays.asList(match)) == null, "old attempt rejected");
        check(ExitDiagnostics.select("pkg:game", "10000 native_enter\n", Arrays.asList(match)) == match, "legacy PID optional");
        check(ExitDiagnostics.select("pkg:game", "bad", Arrays.asList(match)) == null, "malformed stage ignored");
        check(ExitDiagnostics.line(match).startsWith("crash:"), "native crash explicit");
        check(ExitDiagnostics.line(record("pkg:game",77,11000,1,0)).startsWith("os-exit:"), "normal self exit is informational");
        check(ExitDiagnostics.line(record("pkg:game",77,11000,1,86)).startsWith("process error:"), "abnormal self exit not called native crash");
        check(ExitDiagnostics.line(record("pkg:game",77,11000,10,9)).startsWith("os-exit:"), "user force stop informational");
        check(DiagnosticReport.failure(ExitDiagnostics.line(record("pkg:game",77,11000,1,0)), attempt, 11000).isEmpty(), "normal OS exit never triggers launcher error");
        check(DiagnosticReport.failure(ExitDiagnostics.line(record("pkg:game",77,11000,10,9)), attempt, 11000).isEmpty(), "force stop never triggers launcher error");
        check(DiagnosticReport.failure(ExitDiagnostics.line(record("pkg:game",77,11000,1,86)), attempt, 11000).startsWith("process error:"), "abnormal self exit reaches launcher as process error");
        String killed = ExitDiagnostics.line(record("pkg:game",77,11000,2,9));
        check(killed.startsWith("os-exit:") && !killed.contains("low-memory"), "SIGKILL cause not invented");
        check(DiagnosticReport.shouldQueue(killed, attempt, 11000), "SIGKILL queued without calling it a native crash");
        check(DiagnosticReport.shouldQueue(ExitDiagnostics.line(record("pkg:game",77,11000,3,0)), attempt, 11000), "OS low memory queued");
        check(!DiagnosticReport.shouldQueue(ExitDiagnostics.line(record("pkg:game",77,11000,10,9)), attempt, 11000), "user force stop never queued");
        check(!DiagnosticReport.shouldQueue(ExitDiagnostics.line(record("pkg:game",77,11000,1,0)), attempt, 11000), "normal self exit never queued");
        check(DiagnosticReport.failure(ExitDiagnostics.line(match), "10000 loading_libraries\npid=77\n", 11000).startsWith("crash:"), "native library-load crash visible");
        check(killed.contains("rss_kib=456") && killed.contains("reason=2") && killed.contains("status=9"), "raw evidence included");
        File root = Files.createTempDirectory("ss-exit-").toFile(), external = new File(root,"external"); external.mkdir();
        try {
            Files.write(DiagnosticReport.statusFile(root).toPath(), attempt.getBytes(StandardCharsets.UTF_8));
            File log = new File(external,"shiningsource.log");
            Files.write(log.toPath(), "startup\nbackground\n".getBytes(StandardCharsets.UTF_8));
            check(ExitDiagnostics.append(root, external, attempt, match, false), "append matching exit");
            check(!ExitDiagnostics.append(root, external, attempt, match, false), "deduplicated append");
            new File(root,"last-exit-info.txt").delete();
            check(!ExitDiagnostics.append(root, external, attempt, match, false), "log marker deduplicates interrupted receipt write");
            check(!ExitDiagnostics.append(root, external, attempt, record("pkg:game",77,12000,5,11), true), "never append while game alive");
            log.setLastModified(1);
            check(!ExitDiagnostics.append(root, external, attempt, record("pkg:game",77,12000,5,11), false), "never append to stale prior attempt log");
            Files.write(DiagnosticReport.statusFile(root).toPath(), "13000 launch_requested\n".getBytes(StandardCharsets.UTF_8));
            check(!ExitDiagnostics.append(root, external, attempt, match, false), "changed attempt protected");
            byte[] large = new byte[100000]; Arrays.fill(large, (byte)'x');
            Files.write(log.toPath(), large);
            ExitDiagnostics.archive(root, external);
            File previous = new File(external,"shiningsource.previous.log");
            check(previous.length() <= ExitDiagnostics.ARCHIVE_BYTES, "bounded previous session archive");
            check(log.length() == large.length, "archive never changes current log");
            System.out.println("ExitDiagnosticsTest: 28 checks passed");
        } finally { erase(root); }
    }
    static void erase(File file) { File[] children=file.listFiles(); if(children!=null)for(File child:children)erase(child); file.delete(); }
}
