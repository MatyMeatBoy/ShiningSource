# Android compatibility release design and plan

**Goal:** Publish 0.8.8 with the transferable Android startup/compatibility lessons from ReKHCoM, preserving saves and gameplay.
**Reference:** ReKHCoM release source f05f0d3d14eccab20bf6951017610944b217097d, especially `port/android64/host/android64_host.c`.

## Design

The user chose to apply ReKHCoM's relocatable-image approach and broaden the audit to Android compatibility. Keep the current 64-bit host/32-bit-pointer guest architecture. Compile Android images position-independently, retain linker-provided relocation records, reserve a free low-memory image arena, copy and relocate within it, then apply executable permissions. Keep arbitrary Android mappings intact. Preserve desktop builds and cartridge save formats.

Fixed-image loading leaves the reported class of problem open; implementing a full new 32-bit guest/compiler backend would substantially expand this compatibility release. Audit that constraint and report it accurately rather than claim support. ReKHCoM UI/game-specific features are not automatically compatibility requirements.

Add device/build/ABI/page-size/image-slide diagnostics, preserve useful error details, and allow exporting logs without browsing Android/data. Audit SDL renderer fallback, optional audio initialization, lifecycle/input cleanup, storage handling and API-level compatibility; apply concrete improvements supported by comparison and tests.

## Tasks

- [x] Write occupied-image regression and demonstrate the current failure.
- [x] Build relocatable ELF x32 and Mach-O arm64_32 images; validate linker relocation kinds and pointer boundaries.
- [x] Implement bounded image loading and relocation, free-range reservation, W^X code with file-backed executable fallback, and linked-address crash diagnostics. Test forced movement, malformed images and occupied ranges.
- [x] Complete independent Java/SDL compatibility audit; implement applicable findings with behavioral verification.
- [x] Build final APK 0.8.8, run Android 5/14/15, 4/16 KiB, ARM64 and x86_64 cold/moved runs, save restart, renderer/lifecycle/log export checks and appropriate online smoke test.
- [ ] Review final code, check signature/ELF/ZIP alignment/no-ROM packaging, record limitations and checksums. Commit/push tested source, publish a new GitHub release, verify public asset hashes. Carry forward unchanged non-Android packages with explicit versions.

## Review focus

Native pointer relocation versus cartridge constants; relocation-table bounds; 32-bit address overflow; page-aligned permissions and old kernels; unwritable/external storage or missing audio; moved-image save/online determinism; no claim of physical-device verification.

## Execution ledger

Existing 0.8.7 uncommitted stack fix and delivered APK are preserved. User authorized applying the learned Android improvements and releasing a new version; implement inline with independent focused audit/review. No edits to ReKHCoM.

## Scope updates authorized during execution

The user reported a Galaxy A11 rejecting the APK before installation despite running ReKHCoM. Added a real ARMv7/32-bit Android backend rather than just an ABI label: PIC localized guest, softfp bridge, native-thread stack switching and allocated coroutine stacks. Build/fixture linkage pass; actual API21 ARM runtime validation is underway. The user also explicitly requested sharing lessons back to ReKHCoM, including online; a source-pinned handoff is drafted and will be reported with verified results.

This explicit expansion supersedes the initial design's decision to defer a
32-bit backend. The full ReKHCoM comparison is pinned to
6c1951273fc2ccf79369cead1ba207e7297b931d. ARM32 runtime testing exposed and fixed
same-file hook inlining, an overbroad cartridge-address guard, and AAPCS narrow
argument assumptions. Private generated/core/header normalization is covered by
real NDK regression fixtures; no shared runtime ABI is changed.

Interop testing exposed room identity hashing ROM words after native pointer patching. Fixed original-ROM fingerprint capture and bumped protocol rules to6; Windows and Linux are rebuilt so updated peers can meet. A relocated Android guest and Windows delivered23509 shared labels with zero block differences. Linux now uses the rebased PIE guest pipeline; an initial hash mismatch came from omitted SS_SKIP=0 in test setup. With the canonical setting, Linux reaches5F1DB1A8 and the reference audio, with legacy tolerated-access diagnostics retained. No physical-device compatibility guarantee is claimed.

Final ARM32 native payload matches ARM64 in all32000 replay frames and exact
audio. Final APK787816931784af843c120d3a7b049cd15c615a59d7667e18211e0c8e1313af59
passes API21 game-written save/restart and live MCC/MNC injection/restoration;
the latter exposed a missing SDL activity configuration mask and is fixed.
Android11 ARMv7 native bridge passes1200 rendered frames with lifecycle/density
and600 with missing audio. Legacy API21 GLES emulation is unsuitable for graphics,
so native ARM CPU and modern graphics coverage are distinguished. Fifteen Python
and twenty Java checks pass, and final independent review has no open findings.
