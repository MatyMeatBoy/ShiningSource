# Android 0.8.6: memory fix and verification

Subsequent phone reports reached a different startup failure in the fixed stack
window. See [0.8.8](RELEASE_0.8.8.md) for movable images, stacks and service jumps;
the 0.8.6 verification below remains a historical record.

Verified on November 16, 2026. Final APK SHA-256:
`7e65d256a062f0996b374d5111ccdd4e60ae01f7dc17bdf5f9a3e7dabfa8552d`.

## Cause and implementation

The reported error occurs before the ROM is read. The old Android host required
EWRAM at `0x02000000` and assumed the `0x02000000–0x10000000` window was free
when its library loaded. That assumption is not portable across Android
processes. The reporter's particular allocation or kernel restriction is still
unconfirmed because their device log has not been supplied.

The fixture `tools/tests/android_block_gba.h` occupies the original GBA range
and reproduces the exact EWRAM error with the old host. Both Android images now
use `rebase_ir.py` and the existing arena from `console/rebase.c`. Cartridge
pointers retain cartridge values; actual accesses reach the arena. Android's
existing mappings are never replaced. Stacks move above the larger image, and
mapping failures record errno and overlapping maps. Desktop packages retain
their existing memory scheme.

Extended tests found two pointer boundaries that also needed correction:

- Mach-O C names and explicit assembler aliases must normalize to the same
  function, including the allocator's free wrapper.
- The flash-programming callback stored in cartridge memory receives cartridge
  pointers even though the game has no direct declaration of it. Its pointer
  parameter must be converted at entry. Native coroutine callbacks passed
  through attributed native calls retain their existing conventions.

Both boundaries have compiler regression tests that failed before their fixes
and pass afterward.

## Final APK runtime checks

Each row installs the final APK, selects a screen mode, presses **PLAY** on
MainActivity, and runs the canonical 32,000-frame input from a fresh process.
Every frame hash matches across runs and architectures. Each run ends at
`5F1DB1A8`, with zero reached stubs and no allocation failures. Audio:
25,716,797 sample frames, peak 10,279, 35,889,340 samples above the noise floor.

| Android | Guest ABI | Page size | Complete cold runs |
|---|---|---|---|
| 5.0.2 / API 21 | x86_64 | 4 KB | 1 |
| 14 / API 34 | x86_64 | 4 KB | 3 |
| 14 / API 34 | ARM64, native bridge | 4 KB | 3 |
| 15 / API 35 | x86_64 | 16 KB | 3 |
| 15 / API 35 | ARM64, native bridge | 16 KB | 3 |

The final flash callback fix was followed by this complete rerun. Earlier
candidates underwent additional repeated runs, including three on Android 5.

Additional checks:

- The shipped x86_64 image completes the same 32,000-frame test with every
  original GBA address inaccessible. The fixture is injected only into the
  headless host translation unit, never into the APK.
- On Android 15 / 16 KB, both ARM64 and x86_64 save through the game's own
  routine, which returns 0 (written and verified). A fresh process starts with
  the same 64 KiB file intact. Both architectures produce SHA-256
  `de7c407a54edf67e70d71e318aae6eb75dcecc5670f631160cbea4d3d1cdd398`.
  This checks writing, startup and preservation, not restored character
  contents through a manual Continue selection.
- The file picker copies the user's ROM and returns with its SHA-1 verified
  and PLAY enabled.
- ARM64 rendering is visually inspected with on-screen controls. A rendered
  5,200-frame classic candidate run delivered 59.39 game frames/second without
  emptying the audio queue; pictures were skipped in the emulator. The final
  APK's Enhanced rendering is inspected separately. This is not a phone
  performance guarantee.
- All four native libraries have 16 KB ELF load alignment. APK alignment and
  signature checks pass. Version code is 18, version name is 0.8.6, minimum
  SDK is 21, and the signing certificate matches 0.8.5.
- No ROM or save is packaged. The game activity is not exported in the final APK.
- Independent code review found no remaining actionable issues.

## Reproducing

Use a dedicated emulator: these tests deliberately remove its test app's saves.
Supply your own supported ROM; none of the scripts includes one.

- `tools/tests/test_rebased_symbols.py`: compile both pointer-boundary fixtures.
- `tools/tests/android_startup_test.py --help`: final APK cold runs; use `--launcher`.
- `tools/tests/android_memory_test.py --help`: occupied-address regression.
- `tools/tests/android_save_test.py --help`: saving and process restart, after
  the startup test has prepared the app.

Machine-readable results, complete frame logs, screenshots and build output
remain under `build/android/`. Initial emulator storage/System UI failures were
corrected before counting successful runs; these are not game test passes.

## Coverage limits

These checks use emulators; ARM64 runs through native bridges. No physical
Android phone was available. The reporter has been asked for the model,
Android version and log, and their device still needs confirmation. Other
firmware, GPUs, controllers, 32-bit-only devices and every gameplay path are
not covered. The image/stack window still needs to be available to the
32-bit-pointer guest. These checks do not establish universal compatibility
or an absence of all bugs.
