# Android movable stacks implementation plan

**Goal:** Start when Android owns the former fixed stack addresses.
**Architecture:** Keep the linked image's bounds fixed; allocate stacks and bridge at checked free addresses below 4 GiB. Never replace an existing mapping. Desktop non-rebased hosts keep their layout.
**Tech stack:** C mmap, Python regression driver, Android NDK/emulator.
**Spec:** Screenshot reports `the address range for the game's stacks is not free` on the updated Android app.

## Task: Remove fixed stack and bridge requirements

- [x] Inject a fixture occupying the old stacks/bridge and the first proposed allocation range. Run the shipped image; confirm the current host fails on stacks.
- [x] Allocate rebased stacks and bridge safely below 4 GiB, independently of fixed image bounds. Check upper bounds before narrowing pointers. Preserve existing mappings.
- [x] Repeat the occupied-range gameplay regression to frame hash `5F1DB1A8`; verify blocker contents survive.
- [x] Build both Android ABIs, run launcher cold starts and save checks, and record actual coverage. Run compiler regression suite.

## Review focus

Mappings returned above 4 GiB must be released; occupied hints must never be replaced; stack top arithmetic must stay within 32 bits; image loading must retain its original limit; ARM64 and 16 KiB pages must remain supported.

## Execution ledger

Working in the existing clean checkout; no additional worktree needed for this focused repair. Physical reporter log remains unavailable, so this tests the visible failure condition, not the reporter's specific mapping.

Review caught a coarse hint search missing exact-size holes: reproduced exit 3
with only stack/bridge holes available, then replaced the coarse search with a
self-maps gap walk and page-granularity fallback. Exact-hole gameplay now passes.
Android 5's adb shell transport hides remote status: the exhaustion regression
now records native exit status explicitly and passes. Both initial failures and
their causes are described in `docs/RELEASE_0.8.7.md`.

Final APK: `build/android/ShiningSource-0.8.7-android.apk`, SHA-256
`02ede01da35e4c3fad4063eabe87a41061f6cf66a4421859f1052439902bc010`.
Five final launcher runs (Android 5 x86_64, Android 15 x86_64/ARM64), two final
save/restart checks, all four memory fixture modes, two compiler regressions,
signature/alignment checks and final code review pass. No publication performed.
