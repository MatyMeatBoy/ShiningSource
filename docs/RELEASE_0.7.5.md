# ShiningSource 0.7.5

Native port of Shining Soul II (Europe) for Windows x64, Android and Linux
x64. No ROM, BIOS or game data is included: you supply your own cartridge
dump (SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Online, 0.7.5 meets 0.7 to 0.7.4 (nothing in the packets changed). What is new
here is decided by the host's program, so **the host should update first**.

## Online: a change of map while a player's connection is gone

0.7.4 brought a lost connection back, tested in one field. This release
follows the session out of that field, to the world map and into the first
dungeon (`tools/net_outage_map.sh`), with a player cut off on the way.

- **A player given up while the others entered a dungeon** was sent everything
  it had missed and still stayed on the world map for the rest of the session,
  alone. The game starts a dungeon behind a barrier that every player has to
  pass together, and the others had passed it without that player. Now the
  host keeps such a player out and its game is told the session is over,
  instead of leaving it in a world that differs.
- **A player that left for good no longer holds the others on the world map.**
  The game waits there until everybody has chosen the same place; a player
  that will not come back now counts as having chosen. A player that may still
  return is waited for, as the game has it.
- A player the others gave up is forgotten after two minutes, the time it
  waits itself.
- With a fixed delay (`SS_NET_DELAY`, not the default) a returning player's
  program stood still until the host's program ended. It is now told the
  session is over.
- Needed no change: four players, two outages in one session (the same player
  twice, two players at different times or at once, the host and a player at
  once), an outage inside the dungeon.

## Launcher (Windows)

- **Saves page.** Shows your save and the backup of the one before, for the
  normal game and for Rebel mode; keeps dated copies in the folder `saves` at
  the press of a button; puts a copy back after first keeping the present save
  as another copy, so a restore loses nothing.

## Checked

- Three players, a change of map with a player cut off before it (10 and 30
  seconds, a player or the host), after choosing the dungeon (10 seconds: four
  runs, one of them the 64-bit build; 30 seconds: kept out cleanly, three
  runs), a player that never returns, a player that quits. Most cases one to
  three runs.
- Four players: a player cut 30 seconds and the host cut 10 seconds, three
  runs each. Two outages in one session: eleven runs, all whole again.
- The released source, built again: an outage in the dungeon heals, a player
  cut 30 seconds after choosing is kept out as described, the scripted
  32,000-frame play-through gives `5F1DB1A8` and the same sound on the
  development build and Windows x64.
- The Saves page was drawn in a test with a save and a copy present.

## Not checked

- **The Saves page's buttons were not pressed in a test**: copying and putting
  back are a few lines each and were read, not run.
- Fights, items and many enemies appearing during an outage: the scripted
  players reach the dungeon but not its enemies.
- **The end of a level with a departed player.** By reading the game's code,
  the exit counts every player standing ready, so a character left standing
  may hold the others on the level. Not tested and not changed yet.
- What the kept-out player sees on its screen (its program runs on with the
  game's own communication error).
- A real network, a phone, Linux with a window. The Linux x64 package of this
  version was built and not run; 0.7.4 was run in a virtual machine.
- Everything listed as not checked in 0.7.4 and before.

## Install

- Windows: unpack `ShiningSource-0.7.5-windows-x64.zip` and start
  `ShiningSource.exe`.
- Linux x64: unpack `ShiningSource-0.7.5-linux-x64.tar.gz` and run
  `./ss2_linux64 your-rom.gba` (no launcher yet; `LINUX.txt` has the settings).
- Android: install `ShiningSource-0.7.5-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
