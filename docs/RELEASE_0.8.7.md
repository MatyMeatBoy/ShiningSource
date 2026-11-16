# ShiningSource 0.8.7 — Android stack allocation

Historical local test candidate; not published as a GitHub release. Its fixes
are included and extended in 0.8.8.

The updated Android report shows `the address range for the game's stacks is not free`.
This is a native startup failure before the game runs, independent of the online option.

0.8.6 rebased GBA memory but still required fixed addresses for coroutine stacks,
the main stack and service jumps. 0.8.7 allocates these at free addresses below
4 GiB, checking the full range before converting pointers. It first tries a hint,
then searches actual gaps in `/proc/self/maps`; a page-granularity hint search
is available if self maps cannot be read. All these mappings are non-replacing.
The game image retains its existing linked bounds. The early Android reservation
now covers only that image window.

## Regression coverage

The headless Android regression runs the shipped x86_64 image with:

- Former stack/bridge ranges and the first allocation hint occupied: 32,000 frames,
  reference hash `5F1DB1A8`, matching reference audio, blocker contents preserved.
- All compatible space occupied except exact-size stack and bridge holes below
  the first hint: the same gameplay result. This caught a coarse-search bug before
  the gap search was added.
- All compatible stack space occupied: controlled exit 3, without loading the
  game or narrowing a high address into a guest pointer.

Run `tools/tests/android_memory_test.py --help`; the additional modes are
`--block-stacks`, `--stack-hole` and `--exhaust-stacks`. Tests require a dedicated
emulator and the user's supported ROM. No ROM or save is packaged.

## Scope

Final APK SHA-256:
`02ede01da35e4c3fad4063eabe87a41061f6cf66a4421859f1052439902bc010`.

The final APK passed launcher cold runs of 32,000 frames:

| Android | ABI | Page size | Cold runs |
|---|---|---|---|
| 5.0.2 / API 21 | x86_64 | 4 KiB | 1 |
| 15 / API 35 | ARM64 via native bridge | 16 KiB | 2 |
| 15 / API 35 | x86_64 | 16 KiB | 2 |

Each run ends at hash `5F1DB1A8` with the reference audio and no reached stubs.
Both Android 15 ABIs also write and verify the game's 64 KiB save and preserve
it across a fresh process (SHA-256
`de7c407a54edf67e70d71e318aae6eb75dcecc5670f631160cbea4d3d1cdd398`).
This verifies write/startup/preservation, not manual Continue character contents.

The final image passes occupied-original-GBA, occupied-stack and exact-hole
regressions on Android 5.0.2; all-compatible-space exhaustion reports native
exit 3 explicitly (the old adb shell transport does not propagate exit status).
Android 15/16 KiB runs also exercised the occupied-stack, exact-hole and
exhaustion conditions during development. Compiler regression discovery passes
both tests. The APK signature and 16 KiB ZIP/ELF alignment checks pass for all
four native libraries; the signing certificate matches 0.8.6.

Initial emulator setup hit a System UI ANR on Android 15 and an Android 5
boot/install timeout. Restarting System UI and restarting the Android 5 emulator
with software graphics disabled resolved these before successful runs were
counted. Logs/results remain under `build/android/087-*` and the fixture run
folders. Independent code review found and verified the exact-hole search fix,
then found no remaining actionable issues.

This is an Android update (version code 19). Package ID, save location and signing
certificate are unchanged. Install the APK over the previous version.

The reporter's log and a physical-phone confirmation are still pending. Emulator
results cannot guarantee every firmware's memory layout. In particular, the linked
game image still needs its fixed window; this repair removes the separate fixed
stack and bridge requirements.
