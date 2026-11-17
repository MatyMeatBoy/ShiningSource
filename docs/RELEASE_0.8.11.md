# ShiningSource 0.8.11 — item descriptions and Android crash evidence

Item descriptions opened with Select no longer corrupt their glyph upload
cursor when reusing the last text rows. Two row-span loops mixed a changing
start index with an iteration offset and could write past the20 row flags into
the adjacent cursor. This displaced glyph data by one byte, matching the
reported broken letters. Both loops now use stable row indices, correct copy
span starts and bounded row ranges. The fix applies to all rebuilt platforms.

Android preserves the previous session's bounded log before starting another
game. On Android11/API30+, the next launcher visit also records the OS reason
for a matching game-process death, including cases where no native crash line
was written. A kill signal alone is not labeled as proof of low memory.

**Automatic crash reports** is enabled by default and can be disabled in the
launcher. Reports go over HTTPS to the separate VPS receiver on the next
launcher visit, with a bounded persistent queue, deduplication and retry
backoff. ROMs and saves are never opened for reporting; room/network lines,
addresses and private paths are filtered from automatic uploads. Gameplay
does not wait for network delivery. See [details](AUTOMATIC_ANDROID_REPORTS.md).

Online rules remain7, compatible with0.8.10. All players on0.8.8/0.8.9 must
update: the portable scene callback fix from0.8.10 is retained. Experimental
remote combat sounds remain enabled by default.

The second supplied0.8.9 log contained startup and backgrounding only. The user
reported a total Android closure after changing maps in the first dungeon.
That file does not establish a second root cause or prove that0.8.10 failed.
The new diagnostics address the loss of evidence; this release does not claim
to cure every possible termination.

Verification:51 Python tests, including actual patched game functions against
all256 mixed row-flag patterns and byte-exact glyph uploads;73 Java diagnostic
and uploader checks. The five letters of the supplied screenshot's first word
matched the one-byte-displaced original glyphs exactly. Public HTTPS checks
returned health200, new upload201 and duplicate200. An ARM64 Android15 emulator
test recovered a real SIGKILL record, obtained a real HTTPS upload receipt,
preserved that session before Play and distinguished a normal exit.
Final Windows and relocated ARM64 APK builds also matched the canonical
32,000-frame video hash and audio summary. A paired42,000-frame run executed
both callback message types on both peers using portable wire IDs without
crashes. APK signing certificate,16KiB64-bit library alignment and package
payload checks passed.

Android ARMv7/ARM64/x86_64, Windows x64 and Linux x64 packages are rebuilt.
Linux is cross-compiled; physical Android confirmation and the reporter's
exact multiplayer save/route remain pending. No ROM, save or private player
log is included. Thanks to RobNick for the earlier Android compatibility report.
