# ShiningSource 0.5

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Plays online with 0.3, 0.3.1 and 0.4.

## Since 0.4

- **Fast blinking is smoothed.** The cartridge makes things blink by changing
  them every frame or two (the option being confirmed on the title screen and
  in menus, selections, some effects) and counts on a slow screen to melt the
  two pictures together. Where a point of the picture alternates that fast it
  is now shown as the steady mix of the two; anything that simply changes or
  moves is untouched. `SS_SMOOTH=0` turns it off.
- **No bright frame around a dark screen.** In the enhanced mode the field
  beside the picture darkens with the game's own fades and stays black during
  loading screens.
- **The skip-scene fade** takes twice as long, so it is less abrupt.

From 0.4, for those coming from 0.3: skip scene online no longer ends in a
communication error and goes through the whole scene, the item shop no longer
crashes, less latency online (a message of the game measured at 16 ms instead
of 31 on one machine), and the launcher closes when the game starts, with no
console window. See [RELEASE_0.4.md](RELEASE_0.4.md) and
[RELEASE_0.3.md](RELEASE_0.3.md).

## Install

- Windows: unpack `ShiningSource-0.5-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.5-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` on the development
  build and Windows x64.
- The Windows program with a window: starts, shows the title screen, closes.

## Not checked

- The smoothing on the blinks it was written for (the confirmed menu option):
  it was built from how the game draws them, not watched.
- The darkened sides in a wide window during a real loading screen.
- This package on the Android emulator.
- Everything listed as not checked for 0.3 and 0.4, first of all sessions with
  three or four players over the internet.
