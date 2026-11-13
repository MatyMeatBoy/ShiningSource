# ShiningSource 0.6

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Plays online with 0.3 to 0.5, but **the skip of the introduction needs 0.6 on
every player's machine**.

## Since 0.5

- **Skipping the introduction online works, and no longer ends in a
  communication error.** Until now a skip ran the scene through fast. Online
  that never really skipped anything (the introduction is timed, pressing A
  does not shorten it) and, at speed, it ran out every wait the game keeps for
  the other players. The cartridge can end its introduction on Start, but only
  in a single-player game; when every player has voted, the port now takes that
  same exit of the game, on every machine, at normal speed. Measured with three
  players: all in the first field after about 9,000 frames instead of about
  26,000. Online only the introduction can be skipped; alone, every scene as
  before.
- **No more garbage where a sprite blinks.** 0.3 to 0.5 drew a sprite the game
  shows every other frame (the arrow at a door, a character entering or
  leaving) from graphics the game had already reused, which showed as coloured
  noise or a wrongly coloured double. It now uses the graphics and colours of
  the frame the sprite was in.
- **Two things the game alternates in one place** (a fallen character and its
  marker) are both shown, each at half strength, instead of one or the other
  blinking.
- **Companions no longer lost for good by mistake.** Two cases in which a
  player's program gave the others up while they were still playing: when the
  game started its own link task again (it does on some changes of place) the
  port told everybody that the player had left; and when a player's own
  program stood still for a while (its window being moved, a busy machine) it
  counted that time as the others' silence. A player given up stays a standing
  character for the rest of the game, so both showed as companions who stopped
  appearing. The session now simply goes on in both cases.
- **A log of the session**: `shiningsource_net.log`, beside the program, gets
  one line for each thing that happens to the link (a player connected, left,
  timed out, restarted). If a companion stops being shown, the files of the
  players involved say what each program saw.

## Known

- **A companion stopped being shown** to one player in a real session. The two
  causes fixed above are the ones found by reading the code; neither was
  reproduced. If it happens again, `shiningsource_net.log` of the player who
  stopped seeing the others says what that program saw.
- Music was reported silent in multiplayer; the only cause found so far was the
  old skip. Joining a game in progress is not possible yet.
- When player 1 leaves, the session ends for everybody and the game starts
  again at the title.

## Install

- Windows: unpack `ShiningSource-0.6-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.6-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` on the development
  build and Windows x64.
- Three players online on one machine: the voted skip ends the introduction at
  once on all three, no error, all three in the field.

## Not checked

- The skip with three players over the internet.
- The blinking-sprite fix at a door in a real session (the scripted runs do not
  reach one).
- This package on the Android emulator.
