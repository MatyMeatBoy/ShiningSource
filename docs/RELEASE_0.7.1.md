# ShiningSource 0.7.1

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Online, 0.7.1 plays with 0.7: nothing in the link changed.

## Transparency, done again with a test bench

The cartridge fakes transparency by showing a sprite one frame and hiding it
the next. The part of the port that recognises those sprites and draws them
steadily at half strength was rewritten, this time against a bench that
measures the result (`tools/tests/flicker_bench.py`, 49 cases: the old code
failed 27 of them, the new one none).

- A sprite is followed by what it shows, not by its slot in the sprite table,
  so it is still recognised when the game reorders its sprites, when two things
  alternate with each other (a fallen player and the marker over it), and in
  the two-frames-on, two-frames-off rhythm of an enemy appearing or dying.
- A moving sprite keeps its half-strength copy on it instead of leaving one
  behind (the picture at doors, and around a character entering or leaving).
- Smoothing of other blinking only touches pixels whose content stayed in
  place, so nothing that moves is blurred.
- Limits: a sprite that starts to alternate blinks once or twice before it is
  recognised, and one that moves in irregular steps can be a pixel off for a
  frame.

## Sound and pacing

- **A reserve of sound.** The game's samples reached the sound device with only
  a few milliseconds to spare, so any late frame could make a click. There are
  now about 35 ms in reserve, kept there by a speed correction too small to
  hear (0.4%).
- **Display pacing has a floor.** When the display paces the game (60 and
  120 Hz) and the driver returns early (VSync forced off, a minimised window),
  the game no longer runs fast: it falls back to its own clock.

## Also

- Enhanced mode: the border around the picture is drawn once and kept (about a
  thousand rectangles a frame before).
- The per-frame picture work costs less than half of what it did.
- 4 MiB less memory for everybody (a developer's buffer was in every build).
- The picture comes back after a graphics driver reset.
- Rebel mode: a damaged Rebel save with a good backup beside it is restored
  from the backup instead of being replaced by a copy of the normal save.
- `shiningsource_net.log` no longer grows without limit (256 KiB and one older
  file).
- A crash on a thread other than the game's is reported as such.

## Install

- Windows: unpack `ShiningSource-0.7.1-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.7.1-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8`, and the same sound,
  on the development build and Windows x64.
- The transparency bench: 49 cases, none failing. One of them is the game's
  own alternating sprite (the player's body); the others are the same patterns
  put into the sprite table by the bench.
- With a window on a 144 Hz display: 16.74 ms a frame, none late, the sound
  reserve never empty; a simulated 60 and 120 Hz display keeps 16.74 ms.
- The measurements are in [OPTIPORT.md](OPTIPORT.md).
- After the release, from the same source: the play-through gives the same
  hash and the same sound on Linux x64 and Linux ARM64 (Debian 12 virtual
  machines, with a two-player session through a local relay in each), and in
  the Android emulator (x86_64), where the game was also started with its
  picture and left cleanly.

## Not checked

- **The sound was measured, not listened to.** The reserve and the speed
  correction are new: tell us if anything sounds different.
- The transparencies in the scenes the players reported (enemies appearing, a
  fallen companion, doors): the bench reproduces their patterns, not those
  scenes.
- A real 60 Hz display, a real graphics driver reset, a real controller, a
  phone.
- Android on a phone (ARM64): the emulator runs the x86_64 half.
- Everything listed as not checked in 0.7 (combat online, a hosted server).
