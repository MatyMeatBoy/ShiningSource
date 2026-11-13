# ShiningSource 0.7.2

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Online, 0.7.2 plays with 0.7 and 0.7.1: nothing in the link changed.

A release for slower machines, phones above all. Nothing else changed since
0.7.1.

## Speed

- **Drawing the picture costs about a third less.** Backgrounds are drawn a
  tile row at a time and each pixel stops at the first layer that shows
  something. On the development PC: 1.4 -> 0.9 ms a frame in the classic
  picture, 2.1 -> 1.4 ms at the widest. The picture is the same, pixel for
  pixel (see Checked).
- **A machine that cannot draw every picture no longer slows the game down.**
  Until now every frame waited for its picture: on a slow device the game ran
  in slow motion and the sound broke up. Now the game keeps its speed and the
  sound its pace, and pictures are left out instead (never more than three in
  a row). On a machine that keeps up, nothing changes. `SS_FRAMESKIP=0` turns
  it off.

## Install

- Windows: unpack `ShiningSource-0.7.2-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.7.2-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8`, and the same sound,
  on the development build and Windows x64.
- The picture: one hash over all 32,000 pictures of that play-through is the
  same as with 0.7.1 at 240, 284, 427 and 480 columns. The transparency bench
  (49 cases) has no failing case.
- With every picture slowed by 25 ms (a test setting), Windows x64: the game
  ran at 37.9 frames a second before, with the sound reserve empty; now at
  59.6, with 546 of 1,500 pictures left out and the sound never empty.
- 0.7.1, the same source but for the two changes above, ran the play-through
  identically on Linux x64, Linux ARM64 and in the Android emulator.

## Not checked

- **A real phone.** Leaving pictures out was tested by making this PC slow on
  purpose, not on a slow device.
- A display that paces the game at 60 Hz on a slow machine (the test display
  is 144 Hz).
- Everything listed as not checked in 0.7.1 and 0.7: the sound by ear, the
  transparencies in the scenes the players reported, combat online, a real
  controller.

## Known

- While pictures are being left out, blinking that is not a sprite (a menu
  option) may show as blinking instead of smoothed.
