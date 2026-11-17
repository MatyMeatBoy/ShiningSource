# ShiningSource 0.8.14 — Restored Multiplayer Audio

The existing option is now **Audio Multijugador Restaurado** in Spanish and **Restored Multiplayer Audio** in English. All restored sounds use this same option. It remains on by default and retains the saved `remote_sfx` key, live switching and `SS_REMOTE_SFX=0` override.

## Changes

- Hear accepted companion hits against enemies, explicit enemy critical markers and the material-specific impacts on breakable scenery.
- Restore 21 ability sound sites plus transformation/self-buff cues, keeping their original launch, phase and task timing.
- Restore companions' class-specific hurt and falling voices at action transitions; unchanged packets do not replay them.
- Retain the weapon, bow, charge, projectile and thrown-item sounds added in 0.8.9.

Only audio presentation changes. Damage, RNG, resource costs, effect ownership and network packets remain unchanged. Turning the option off restores the original exclusions; local single-player audio is preserved. See the [source audit](MULTIPLAYER_AUDIO_AUDIT.md) for event mappings and intentionally unmodified paths.

The shared narrow-map fading fixes from 0.8.13 are now included in the Android and Linux builds too. Windows retains the official local achievements introduced in 0.8.12.

## Verification

The compiled audio fixtures cover accepted remote enemy hits, critical markers, breakable-object impacts, 22 ability/transform sound guards, self-buff resource/effect ownership and hurt/death transition deduplication. Existing attack/bow/charge tests, callback-boundary checks, renderer, text, font, Android image and network code-pointer regressions pass: 40 tests in total.

The Windows canonical replay completes 32,000 frames with the unchanged video hash `5F1DB1A8` and audio reference: 25,716,797 sample frames, peak 10,279, 35,889,340 samples above the noise floor. The options menu was rendered and visually inspected with the full Spanish label; live off/on changes persist across fresh processes using the existing key.

Two-client Windows on/off sessions include real combat as a positive control. In the final runs, attack cue `0x14` increased from 13 to 22 calls and enemy-impact cue `0x1A` from 18 to 24 calls. Both sessions complete 27,000 frames and their peers agree on 26,509 shared labels per session, with zero shared-block mismatches. Counts are aggregate calls from both clients, not subjective listening scores.

Windows, Android (ARMv7, ARM64 and x86_64, API21+) and Linux x64 are built from this revision. The APK is versionCode 24/versionName 0.8.14, verifies with the same certificate as 0.8.11, and passes 16 KiB alignment and content checks. Windows/Linux archives are packaged from explicit allowlists with verified SHA-256 manifests. Android and Linux runtime listening, every weapon/effect, every physical device and four-player audio remain unverified; no device was connected for this release's Android runtime checks.

Online rules remain 7. Saves, package ID and Android signing identity are unchanged. Packages contain no ROM, saves, private settings, credentials or diagnostic logs.
