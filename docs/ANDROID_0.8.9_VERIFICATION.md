# 0.8.9 verification record

The European cartridge dump used for tests remains private. Fixture font files
are synthetic. No cartridge, save, generated glyphs or RAM dumps are published.

## Audio behavior and network

- Six compiled-C audio regressions exercise actual extracted game branches and
  the real policy module: default on, environment override, live setting,
  four actors, unchanged packet deduplication, class-specific layered sounds,
  bow freeze, projectile/charge/throw guards and unchanged actor bytes.
- Replaying the original unpatched source makes the four game-path scenarios
  fail. A guarded setter/getter fixture detects host-service calls inside the
  reverse option callback; the diagnostic implementation failed before removal.
- Two Windows clients ran27000 frames in a local relay session with the option
  off, then on. Each session had26509 shared labels and zero block differences.
  In the starting encounter, weapon cue0x14 calls increased from8 to18 across
  the two clients. This observes calls into the actual sound engine, not a
  physical listening test or an exact count guarantee for every playthrough.
  `tools/tests/remote_sfx_online_test.py` requires combat as a positive control,
  so a lobby-only run with no attacks cannot pass.
- Windows, ARMv7 Android and ARM64 Android offline canonical replay reaches32000 frames,
  video hash`5F1DB1A8` and the unchanged audio reference:25716797 sample frames,
  peak10279,35889340 samples above the noise floor.

## Options and fonts

- Windows options tests exercise default on, live guest callback acknowledgment,
  off/on persistence across fresh processes and environment-over-file priority.
- Final ARMv7 APK on Android11 through native bridge:600 rendered frames and
  off/on changes across fresh launches. Callback replies and the saved option
  agree. A longer held-menu run was visually inspected with the real ROM font
  and the highlighted experimental option. It exits normally.
- ARM64 final APK on Android15 with16KiB pages also passes the replay and the
  rendered menu/callback test with forced image relocation and memfd execution.
- A menu test exposed an existing fixed-ROM font read at0x080000AC. The final
  bounded reader is covered by nine synthetic-file tests: valid content,
  malformed headers/offsets, overflow, truncation, nonempty space and missing
  file fallback. The original unmapped-address failure and successful menu runs
  are preserved locally.
- The Android11 test emulator uses root only to inject disposable app files
  with correct ownership/SELinux labels. The APK requires no root or relaxed
  security setting.

Thirty Python tests pass. Independent reviews cover audio-only patch scope,
setting arrays/callbacks, callback purity and the bounded font reader. Android
signing/alignment/ELF and public artifact hashes are checked before delivery.
Linux is rebuilt; this update does not claim a new Linux runtime test.

Final APK SHA256:
`65bc3547cf20d0643b9a2909e3d20f0bb10eafee9ced6a22077d820ce3811b08`.

## Limits and reporting

The user's successful Android/Windows connection is a field report separate
from these fixtures and local sessions. Device model, ABI, Android version,
session length and four-player behavior are unknown. Confirmation of companion
audio on physical Android/Windows devices remains pending. No claim of universal
compatibility or every weapon/effect on every platform is made.
