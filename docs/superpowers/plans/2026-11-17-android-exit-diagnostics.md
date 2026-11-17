# Android missing exit evidence

The second supplied log is 846 bytes from Android0.8.9, containing startup and
background lifecycle only. No crash marker, signal, native PC or exit code is
present. Backgrounding alone is not proof of a crash. Do not claim a cure for
this unknown termination.

Confirmed diagnostic defect: a new game truncates shiningsource.log and Java
startup status, discarding evidence from the previous session. Preserve a
bounded previous-session copy before replacing either. On API30+, collect OS
exit information for the matching game process and attempt, never a launcher,
live game or older attempt. Append a bounded deduplicated reason to the usual
log. Distinguish explicit crashes, nonzero exits and informational user/system
terminations. Keep API21 compatibility and no new permissions/network requests.

- [x] Plain-Java matching, classification and preservation regressions.
- [x] Build three-ABI Android0.8.11; retain online rules7 and PC0.8.10 compatibility.
- [x] Emulator real process termination/relaunch and normal exit checks.
- [ ] Publish with an explicit diagnostic-only scope and unknown-cause limitation.

The user subsequently reactivated VPS uploads and authenticated Easypanel.
The separate receiver and Android client are implemented and tested, including
real HTTPS receipt after emulator process death. No private player log or game
asset is distributed. Item-description rendering was added to the hotfix scope
and is still being investigated independently; do not claim it fixed yet.
