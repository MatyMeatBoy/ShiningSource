# ShiningSource 0.7.3

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Online, 0.7.3 plays with 0.7, 0.7.1 and 0.7.2: nothing in the link changed.

A correction to 0.7.2, for slow devices only. On a machine that draws every
picture in time nothing changes.

## Fixed

- **0.7.2 left out far too many pictures on a slow device.** Once it had left
  one out, the next picture waited for a moment that had been pushed into the
  future: three of four pictures went missing (15 a second) and the display
  was no longer used to pace the game. Found by running 0.7.2 in the Android
  emulator, which is such a slow device. Now only as many pictures are left
  out as it takes to keep the game at its speed.

## Checked

- Android emulator (x86_64, software graphics), 1,500 frames with the picture:
  the game ran at 42 frames a second when no picture is ever left out, with
  the sound reserve at 1.7 ms; with 0.7.3 at its normal speed, showing 46
  pictures a second, the sound never empty. The scripted 32,000-frame
  play-through gives `5F1DB1A8` and the same sound there too.
- Windows x64: the play-through gives `5F1DB1A8` and the same sound; with
  every picture slowed by 25 ms the game keeps 59.6 frames a second (37.9
  without leaving pictures out); at normal speed no picture is left out.

## Not checked

- A real phone (ARM64). The emulator runs the x86_64 half of the package.
- Everything listed as not checked in 0.7.2, 0.7.1 and 0.7.

## Install

- Windows: unpack `ShiningSource-0.7.3-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.7.3-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
