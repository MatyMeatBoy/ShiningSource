# ShiningSource 0.8.5

Shining Soul II (Europe) as a native program for Windows x64, Android and
Linux x64, and, for the first time, a package for the PS Vita that has never
been run. What the port is and does is in the [0.8 notes](RELEASE_0.8.md).

No ROM, BIOS or game data is included: you supply your own cartridge dump
(SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). Online, 0.8.5 plays with
0.8.4: nothing in the link changed.

## New

- **PS Vita, untested alpha** (`ShiningSource-0.8.5-vita-UNTESTED.vpk`). A
  console cannot give the game the Game Boy Advance's memory at its real
  addresses, so the game's memory lives inside the program and every access is
  redirected to it. That scheme is proved on a PC: a Windows program built
  with it, with the real addresses made inaccessible, gives the reference hash
  and sound. The Vita package is built with the Vita SDK and accepted by its
  packaging tools. **It has never run: not on a Vita, not on any ARM
  processor.** It may not start. No options menu, online or achievements in
  it. To try it (HENkaku / Ensō): install the package with VitaShell, copy
  your own ROM to `ux0:data/ShiningSource/` (any name ending in `.gba`),
  start it, and send us `ux0:data/ShiningSource/log.txt`. A dark red screen
  for five seconds means no ROM was found. Circle is A, Cross is B.
- **Android: the start screen no longer offers a delay.** A number there
  selected the older way of playing online, in which everybody waits for the
  slowest player. Android now always uses the direct one, as the PC does.
- Sprite rotation takes its sines from a table of whole numbers, so every
  processor computes the same values.

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` and the same sound
  on the development build, Windows x64 (from the packaged files) and the
  program built with the consoles' memory scheme.
- Android emulator against a Windows player through a local relay (before the
  delay field was removed, with no delay set): the session is joined and
  played at 59.4 game frames a second with the picture shown, the sound never
  empty.
- The ARM64 half of the Android package, in the emulator's ARM translation:
  every frame hash equals the reference up to frame 3000, where the
  translation stops because it cannot deliver the memory faults the port
  tolerates (a real phone can; the Linux ARM64 virtual machine runs the whole
  play-through).

## Not checked

- **The Vita package: nothing of it has run.**
- The Android and Linux packages of this version were built, not run again
  (0.8.4's were, and the code they share with Windows passes here).
- Everything listed as not checked in 0.8.4 and 0.8.

## In progress, not in this version

- PS4, PS5 and Switch builds; the party's defeat online with a player gone;
  removing the game's stray memory accesses at their source.

## Install

- Windows: unpack `ShiningSource-0.8.5-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.8.5-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
- Linux x64: unpack `ShiningSource-0.8.5-linux-x64.tar.gz` and run
  `./ss2_linux64 your-rom.gba` (no launcher yet; `LINUX.txt` has the settings).
- PS Vita: see above.
