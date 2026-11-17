# Restored multiplayer audio — 0.8.14

User scope: audit missing single-player cues in multiplayer, especially enemies hit by companions; restore confirmed omissions under the existing option and rename it Audio Multijugador Restaurado. Publish a new version after verification. Keep the saved remote_sfx key and existing environment/bridge settings.

Design: reuse accepted hit/critical/object events and the game's existing replicated ability tasks. Only audio calls gain online/option permission. Do not broaden damage, resource costs, collision ownership, RNG or network packets. Keep world/map, phase and existing task lifecycle guards. Enemy critical marker A1 is distinct from the hit packet's high-bit flag, which must not be guessed to mean critical.

Plan:
- [x] Inventory audio entry points and local/remote guards; document restored, already replicated, local-only and unsupported cases.
- [x] Demonstrate omissions with compiled source-derived fixtures; patch accepted peer impacts/critical markers and reachable ability sound gates.
- [x] Restore additional confirmed object/buff presentation cues without changing local gameplay branches; include deduplicated companion hurt/falling voices.
- [x] Rename the existing option, compile and run source tests, offline reference replay and real on/off multiplayer traces with positive hit controls.
- [x] Build supported release platforms, review changes and prepare the allowlisted 0.8.14 packages for publication.

Evidence and limits belong in the release notes. No claim of every weapon on physical hardware follows from synthetic branch tests.

Final checks: 40 tests pass. Final two-client sessions complete 27,000 frames: attack calls 13→22, enemy impacts 18→24; 26,509 matching labels per session and zero mismatches. Offline 32,000-frame video/audio references are unchanged. Live Spanish option layout and off/on persistence pass. Windows and all three Android ABIs build; Linux x64 cross-build passes. APK certificate matches the previous release; version24/0.8.14, 16KiB alignment and content checks pass. Independent reviews found no release blocker, including the two companion voice additions. Publication follows the release commit; the GitHub release and uploaded digests are authoritative.
