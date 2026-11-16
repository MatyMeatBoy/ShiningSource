# ShiningSource 0.8.8 — Android compatibility

Thanks to **RobNick** for reporting the Android startup failure. His screenshot
identified `the address range for the game's stacks is not free` after the
earlier EWRAM correction. Confirmation on the reporting phone is still pending.

This update applies Android memory and startup lessons from the more mature
ReKHCoM port. Windows and Linux also receive the architecture-independent online
room identity correction described below. Linux shares the rebased PIC image
loader and no longer requires changing `mmap_min_addr`. The carried-over Vita 0.8.5 package
remains an untested alpha with the older online protocol; it cannot join 0.8.8 rooms.

## Changes

- A native ARMv7 backend adds support for 32-bit Android. Its private rebased game
  object, Android softfp ABI and stack-switching bridge avoid fixed GBA addresses
  and keep calls into Android/ART on the native thread stack.
- Android can relocate the complete game image when its linked address is
  occupied. Both ARM64 and x86_64 images are position-independent and carry
  explicit linker relocation records. Only native pointers are relocated;
  cartridge addresses retain their original values.
- Coroutine stacks, the main stack and the host service bridge use available
  memory below 4 GiB instead of requiring their old fixed addresses. The allocator
  searches actual gaps, checks pointer limits and never replaces Android mappings.
- Code is written before it is made executable. Code/data permissions are
  separated on 4 and 16 KiB pages, with a memory-file-backed executable fallback
  when anonymous executable memory is unavailable. Fixed replacement is used
  only inside allocations already owned by this host.
- Startup logs identify the app version, device, Android version, ABI, page size,
  image placement and relocation offset. Crash diagnostics retain linked game
  addresses for symbol lookup. Malformed or unsupported images fail explicitly.
- **Share report** on the initial screen exports bounded device/startup/native
  diagnostics through Android's share chooser. No access to Android/data is
  required, and no message is sent automatically. Reports exclude ROMs, saves
  and settings files. Java-side library/startup failures survive recreation.
- The launcher distinguishes successful exits, soft resets, incomplete starts
  and explicit failures. The SDL game activity handles SIM/operator MCC/MNC and density changes without
  rebuilding SDL in the same live process; the launcher can recreate for language
  and font changes.
- An unavailable optional audio driver no longer prevents the game from starting.
  Background/foreground handling clears stale input/audio and resets pacing on
  resume without adding a separate game/network pause policy.
- Online rooms now use a fingerprint captured from the original ROM before its
  function pointers are patched. Previously their identity could depend on native
  addresses, separating identical cartridges on different architectures or after
  relocating the image. **All players must update to 0.8.8**: protocol rules 6
  intentionally isolate older clients instead of mixing incompatible identities.

## Installation and scope

Install `ShiningSource-0.8.8-android.apk` over the previous version. Package ID,
signing certificate, supported ROM and save location are unchanged. No ROM or
save is included. Android 5/API 21 or later with **ARMv7, ARM64 or x86_64** is required.
The new native ARMv7 backend covers 32-bit Android installations, including phones
whose CPU supports 64 bits but whose Android installation only accepts 32-bit apps.
The Galaxy A11 install rejection reported during this work motivated this addition;
confirmation on that physical phone is still pending.

The underlying SDL renderer selection, renderer-reset handling, controller
hotplug, Back/menu behavior and SAF ROM picker were retained after comparison.
ReKHCoM's internal-storage architecture was not copied without a save migration:
the app continues to use its existing app-specific storage and reports if that
storage is unavailable.

Testing uses emulators; ARM64 runs through their native bridge. It does not
establish compatibility with every physical phone, firmware, GPU or audio route.
All game allocations must still fit in the guest's 32-bit virtual address space;
extreme fragmentation can cause a controlled startup failure. Other gameplay
limitations in the earlier release notes still apply.

## Verification

The release verification record and final SHA-256 are recorded in
`docs/ANDROID_0.8.8_VERIFICATION.md` and the release checksum asset.
