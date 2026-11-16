# 0.8.8 verification record

Tests use a privately supplied European cartridge dump; no ROM or save is
distributed. Emulator results do not certify every physical device. RobNick's
phone and the reported Galaxy A11 still require confirmation after installation.

## Reference behavior

The canonical `tools/tests/play_input.txt` replay uses `SS_SKIP=0`, 32000 frames,
final video-state hash `5F1DB1A8`, and audio statistics of 25716797 sample frames,
peak 10279, 35889340 samples above the noise floor. These checks exercise a
specific sequence, not every game state.

## Verified coverage

- Windows x64 and Linux x64: canonical final hash and audio match. Linux uses
  the rebased PIC image loader; legacy tolerated-access diagnostics remain.
- Android 14 x86_64, 4 KiB pages: final universal APK, forced image relocation
  and executable-memory fallback, canonical final hash/audio match.
- Android 15, 16 KiB pages: ARM64 through native bridge and native x86_64,
  forced relocation/memfd, canonical hash/audio and game-written save/restart.
  ARM64 replay/save was repeated after the final ARM32 build; x86_64 also passed
  Android 14 with the universal APK. Later MCC/MNC manifest changes leave all
  six native libraries, guest assets and Java bytecode byte-identical.
- Android 15 ARM64 rendered lifecycle: 1200 frames, screen off/on and density
  change, process survives and real SDL watcher transitions are recorded.
- Android 15 ARM64 unavailable audio driver: 600 rendered frames, continues
  without audio. Share-report chooser inspected; no message sent.
- Memory fixtures: occupied linked image and historical stacks, exact-sized
  low-memory gaps, forced memfd, and controlled failure on exhaustion.
- Online: relocated Android x32 guest and Windows, 24000 frames each,
  23509 shared labels with zero recorded block differences. This verifies the
  corrected room identity and that sequence, not every recovery scenario.
- Portable image parser, ARM32 build/ABI/hook routing, original-ROM identity,
  and bounded Java diagnostics have automated regression checks.

## ARM32 and intermediate-state verification

Android 5/API21 ARMv7 full CPU emulation completes 32000 frames with the reference
final hash and exact audio. All 32000 frame hashes match the ARM64 trace after
the final private runtime build. A strict comparison originally found hidden
OBJ/OAM differences at frames 3057–3146 despite identical black rendered output
at the first differing frame. The cause was a mismatched wide caller/narrow
callee argument, corrected with explicit callee normalization in both generated
game code and private runtime/header copies. NDK fixtures cover that failure.

The game-written ARM32 save/restart check preserves the same 65536-byte save as
ARM64/x86_64: SHA256
`de7c407a54edf67e70d71e318aae6eb75dcecc5670f631160cbea4d3d1cdd398`.
The save/restart check was repeated with the final APK (`088-release21-save`).

Replay receipts: `088-release21-arm32` and `088-release35-arm64` use APK
`7a5a666d6beb2914b21b514cdb2a99741f188093dbfd7a971f26db20ea68f35d`.
The final MCC/MNC manifest-only APK is
`787816931784af843c120d3a7b049cd15c615a59d7667e18211e0c8e1313af59`;
payload identity was checked before running final configuration tests.

## Final APK configuration and graphics

- `088-release21-mcc-replay`: final APK on API21 ARMv7, 32000 frames, reference
  hash/audio. During gameplay `088-release21-mcc` injects MCC/MNC 311/481 and
  restores the original values, independently rereading system configuration;
  game PID 1666 survives both changes. The original missing-mask failure was
  observed in a boot-time SIM configuration change and preserved in logcat.
- `088-release30-render`: final APK on Android 11 x86 with ARMv7 native bridge,
  1200 real rendered frames, screenshot inspected, actual background/foreground
  watcher transitions and density change; process survives. The legacy API21
  emulator's GLES drivers were unsuitable for this graphics test, so API21
  coverage uses ARM CPU emulation and graphics coverage uses the modern bridge.
- `088-release30-noaudio`: final ARMv7 APK renders 600 frames with an invalid
  audio driver and exits normally, confirming the optional-audio fallback.
- Android 11 scoped-storage test injection requires root only on the dedicated
  disposable emulator to set test-file ownership and restore SELinux labels.
  No app permission/security requirement is relaxed by the APK.

## Artifact checks

Final APK SHA256:
`787816931784af843c120d3a7b049cd15c615a59d7667e18211e0c8e1313af59`.
Signature and ZIP alignment verify; signing certificate is unchanged. All six
libraries have no TEXTREL. ARM64/x86_64 LOAD alignment is 16384, ARMv7 is 4096
(the NDK's normal 32-bit alignment). SDK minimum21, target35, versionCode20,
versionName0.8.8, three declared ABIs. No cartridge or save is packaged.

Fifteen Python compiler/parser/identity regressions and twenty Java diagnostic
checks pass. Final code received independent review; discovered issues were
fixed before closing their gates. Release SHA256SUMS records the four platform
packages; Vita remains an unchanged untested older package.
