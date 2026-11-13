# ShiningSource 0.4

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Plays online with 0.3 and 0.3.1 (the link is the same), but everybody should
move to 0.4: the fixes below are on each player's own machine.

## Since 0.3.1

- **Skip scene online no longer ends in a communication error.** Skipping ran
  the game as fast as the machine allowed; online the game counts its waits
  for the other players in frames, and at that speed they all ran out at once.
  Online a skip now runs at twice the normal speed, which those waits can
  take. Alone it is as fast as before.
- **A skip goes through the whole scene**, not one paragraph (0.3 stopped at
  the first change from one kind of scene to the next).
- **No dark wait.** A player who reaches, before the others, a point where the
  game waits for them gets the picture back instead of a black screen.
- **The item shop no longer crashes.** It crashed every time it was opened,
  alone or online: one of its functions used the console's stack directly,
  which the port's build had left pointing nowhere. Two more functions written
  the same way (item synthesis, one scene's camera) are fixed with it, and the
  build now refuses such a function without a fix.
- **Less latency online.** What a frame of the game writes is sent at the end
  of that frame instead of at the start of the next, and a message of the game
  no longer spends a frame waiting before the next one may go.
- **Enemies** are corrected on the other players' machines three times a
  second (0.3: once; the cartridge: every five seconds). What an enemy does is
  still sent the moment it changes.
- **Launcher**: Play saves the settings, starts the game and closes; no console
  window appears beside the game any more; a "Save settings" button confirms
  them at any time.

Everything else is 0.3: see [RELEASE_0.3.md](RELEASE_0.3.md).

## Install

- Windows: unpack `ShiningSource-0.4-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.4-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` on the development
  build and Windows x64.
- The item shop opens and lists its goods on both builds (it crashed before).
- Two players online on one machine: a voted skip of the introduction runs to
  its end and both reach the game.

## Not checked

- A skip with three players over the internet, which is where the
  communication error was reported: the cause above is the one found by
  reading how the game waits, not one reproduced with three machines.
- The shop reached by walking to its seller in town (the test opens it with
  the game's own opener), the synthesis menu and the fixed camera scene.
- Everything listed as not checked for 0.3 and 0.3.1.
