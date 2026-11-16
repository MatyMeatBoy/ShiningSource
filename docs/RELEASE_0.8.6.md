# ShiningSource 0.8.6 — Android startup fix

An Android update for the ROM-loading failure reported in 0.8.5:
`the address range for the game's EWRAM (0x02000000) is not free`.

No ROM, BIOS or cartridge data is included. Supply your own Shining Soul II
Europe dump (SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). Install
`ShiningSource-0.8.6-android.apk` over the existing app; its package ID and signing
certificate are unchanged. Saves keep their existing location.

## Fixed

- Android no longer asks for EWRAM, IWRAM, video memory or the ROM at the GBA's
  original hardware addresses. Both Android guest images use the port's existing
  rebased memory scheme, with cartridge pointers translated into the game's own
  arena. Memory owned by Android is never overwritten.
- ARM64 allocator wrappers translate pointers consistently when calling a game
  function with an explicit assembler alias. The extended gameplay test caught
  a missed translation on free; a compiler regression test now covers it.
- The flash-programming callback also translates the cartridge pointer it
  receives indirectly from the game. A separate save test caught this path:
  the game's own routine must write and verify a 64 KiB save, and a fresh
  process must start with that file intact.
- Mapping failures report the actual system error and overlapping mappings.
  The checked, non-replacing fallback also supports older kernels that reject
  `MAP_FIXED_NOREPLACE`. No root access or change to `vm.mmap_min_addr` is needed
  for the reported EWRAM problem.

## Runtime verification

The final APK is installed and launched through the first screen's **PLAY**
button. Each scripted run lasts 32,000 game frames and must finish at hash
`5F1DB1A8`, with the reference audio statistics and no allocator failures.

The detailed Android version, ABI and page-size matrix is recorded in
[the verification report](ANDROID_MEMORY_FIX.md). A separate test occupies the
entire original GBA address range before loading the shipped image: the old
host reproduces the report, and the new host must complete the same gameplay
test while those addresses remain inaccessible.

The APK's four native libraries have 16 KB ELF load alignment. The final APK,
including its game images, is installed for the runtime checks.
No test-only exported game activity is enabled in this APK.

## Limits and other platforms

The Android runs use emulators. ARM64 is exercised through their native bridge,
not a physical phone. The reporter has been asked for their device model,
Android version and log; that particular device has not yet been verified.
These checks do not establish compatibility with every Android device or an
absence of all bugs. Only 64-bit ARM and x86-64 devices are supported.

This update changes Android. Windows and Linux remain at **0.8.5**; their
existing 0.8.5 packages are carried over unchanged for convenience. The Vita
0.8.5 package remains an **untested alpha**. The other known limitations in
[0.8.5](RELEASE_0.8.5.md), [0.8.4](RELEASE_0.8.4.md) and [0.8](RELEASE_0.8.md)
still apply.
