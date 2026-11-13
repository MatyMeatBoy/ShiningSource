# ShiningSource 0.7

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

**Online, 0.7 only meets 0.7.** Every player in a room needs this version.

## Online

- **One order of messages for everybody.** Until now each player's messages
  arrived in order, but how they interleaved with the other players' differed
  from machine to machine. Every message now goes through player 1 and every
  machine acts on them in the same order.
- **Enemies that only some players had.** The game can start two enemies at a
  time; several arriving together lost the third on that machine: an enemy the
  others saw and it did not. They wait their turn now.
- **Less latency**: what a burst of the game's messages needs (a map entry,
  several enemies acting) leaves and is acted on in one frame.
- **More ways to meet**, on the launcher's Online page: the official server
  (the default), *Host on this PC* (starts the server that comes with the
  port and lists this PC's addresses with a Copy button) and *Connect by IP*
  (paste the host's address). To host for players outside your own network,
  UDP port 47900 has to reach your PC.
- The display paces the game online too (since online became the default, a
  60 or 120 Hz display was repeating a frame every few seconds for everybody).

## Sound

- **The sound engine was rewritten after the cartridge's own driver.** A song
  that is stopped and asked for again continues instead of starting over (the
  game does that at every room change and menu close); the Game Boy style
  channels sound as the driver makes them; samples that played 27% sharp are
  in tune; vibrato and reverb are there. All 74 pieces of music and the
  effects pass an audit of the whole song table.
- **Music in multiplayer.** In a game started from the multiplayer lobby the
  cartridge plays music only in the town and the first field: every other
  stage is silent by the game's own choice. The port lets each stage play the
  song it has in a single-player game (`SS_MP_MUSIC=0` gives the cartridge's
  silence back).

## Inside the game

- **Options menu**: Escape, Select + Start on a controller, or Back on Android.
  Screen mode, border or sides, full screen, VSync, volume, smoothing, skip
  scene, quit; each applies at once and is remembered. Alone the game is
  paused; online it runs on behind the menu. Escape no longer closes the game.
- **The port's own texts use the game's typeface**, read from your ROM while
  you play.
- **Skip scene covers long dialogue sequences** (the guide in the first field,
  and the others told the same way). It stops at a question instead of
  answering for you. Online it is a vote, and the windows stop waiting instead
  of anything running fast.
- **Experimental** (launcher, page *Quality of life*, off by default): LT / RT
  step the L and R bars backwards; D and F on the keyboard.

## Picture

- Correct pacing at every refresh rate: a multiple of 60 Hz paces the game,
  any other rate from 59 Hz up keeps the game's clock with VSync, and a slower
  display no longer slows the game down.
- The half-strength copy of a sprite the game alternates moves on with it, so
  a label over a walking companion is no longer blurred; it also follows the
  camera.
- Where a map ends beside the view the fade is wider and curved; the sides
  around a dark screen sink and rise with the game's own fade.

## Also

- Android: the game no longer aborts on its way out.
- The Android package always carries both game images built from the same
  source.

## Install

- Windows: unpack `ShiningSource-0.7-windows-x64.zip` and start
  `ShiningSource.exe`. `ss_relay.exe` in the same folder is the server that
  *Host on this PC* starts.
- Android: install `ShiningSource-0.7-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8`, and the same sound,
  on the development build and Windows x64.
- Online on one machine, 2 to 4 players with latency and loss simulated:
  every machine acts on the messages in the same order in every session; the
  voted skips of the introduction and of the guide's dialogue work for three
  players.
- The options menu with a window: opens, pauses, changes take effect and are
  written back.
- The sound audit of all 428 entries of the song table.

## Not checked

- **Combat online with the new order.** The scripted sessions do not reach a
  place where enemies die and respawn, so the two fixes for enemies rest on
  reading the game's code. A real session is the test.
- Music by ear: everything about sound was measured, nothing listened to. The
  area the players reported silent was not reached by a scripted run.
- A game played through a server hosted from the launcher.
- A real controller (the menu's Select + Start, LT / RT).
- This package on Android (0.6 was run in the emulator).
- Sprites the game alternates, in the scenes the players reported: a test bench
  for them is being built.

## Known

- Joining a game in progress is not possible.
- When player 1 leaves, the session ends for everybody and the game starts
  again at the title.
