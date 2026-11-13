# ShiningSource 0.8

The first public release. Shining Soul II (Europe) as a native program for
Windows x64, Android and Linux x64: widescreen, online for 2 to 4 players,
achievements. The versions before it (0.1 to 0.7.5) were test builds for a
small group of players.

No ROM, BIOS or game data is included: you supply your own cartridge dump
(SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Online, 0.8 meets 0.7 to 0.7.5 (nothing in the packets changed), but what is
new is decided by the host's program and by each player's own. **Play 0.8
with 0.8.**

## What you get

- **The game itself**: every function of the cartridge is the decompilation's
  C, compiled unchanged for your machine. Not an emulator and not a static
  recompilation. Saves work as on the cartridge, with a backup of the save
  before the last one.
- **Picture**: the console's picture at a whole scale inside a border, or a
  wide view that shows more of the map (up to twice the console's width)
  without stretching anything, with the status display at the screen's edges.
  Sprites the cartridge blinks to fake transparency are drawn at half
  strength instead. VSync, variable refresh, or neither.
- **Online for 2 to 4 players** through a small relay (a public one is the
  default; `ss_relay` is included to run your own): the game's own
  MULTIPLAYER MODE, with crossplay between Windows, Linux and Android.
  Nobody waits for a slow player. A player whose connection drops is taken in
  again and receives what it missed; a player who leaves stays as a standing
  character and the others play on.
- **Comfort**: skip scenes with Start (a vote online), every key and
  controller button reassignable, touch controls on Android, an options menu
  in the game (Escape, or Select + Start), English and Spanish.
- **Achievements**: the RetroAchievements set of your ROM is followed while
  you play (sending unlocks to your account is off by default), the port's
  own twelve, and Rebel mode with cheats and its own save.
- **Launcher** (Windows): checks your ROM, all settings, online, dated copies
  of your save.

## New since 0.7.5

- **A player that is gone no longer holds the others in a map.** With three
  players and one program closed in the first field, the other two could not
  leave it: the scene there waits for every player's arrival. Measured with a
  scripted session and fixed; the others now go on through every change of
  map of that session, whether the player crashed or quit. The same rule was
  applied, from reading the game's code, to the exits of a level, the portal
  back, the boss portals and the party's defeat.
- **The item shop, proven.** Players reported a crash on opening a shop in
  multiplayer and no crash record was ever sent. A scripted route now walks
  to every counter of the town, alone and with two players: on a build without
  the 0.4 correction the item shop crashes exactly as reported, and on this
  version it sells, buys and closes. The weapon and armour shops, the
  blacksmith, the appraiser, the storage and the Start menu's pages are
  walked the same way.
- **Music.** Every one of the game's 399 songs was played through the sound
  engine in a survey: none silent, no track lost, no command the engine does
  not know. The areas reported without music are therefore a matter of when
  the game asks for a song. One such case is changed: a song asked for again
  while it was still fading out was left to fade into silence (on the
  cartridge the fade ends while the next map loads); it now starts again.
- **Joining a game in progress, as an experiment.** Off by default
  (`SS_NET_JOIN=1` for every player; they meet in a room of their own). A
  program started later is let in when the party enters a dungeon map, and
  follows it from there. Nobody is let in while the party stays in the town.
  `docs/JOIN_IN_PROGRESS.md` has the mechanism and its limits.
- Linux x64 is a package of the release (`.tar.gz`, started from a terminal).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` and the same sound
  on the development build and Windows x64, from the packaged files too.
- A player gone: three players with one program ended in the first field (the
  others were stuck before, go on now), ended in the next area, one that
  quits, two players with the client gone (the host goes on alone). One run
  each; one repeated on the released source.
- Online regressions on the released source: three players with a player cut
  off 30 seconds (whole again), a newcomer joining two players (joined), a
  session of three on the 64-bit build through four changes of map.
- The town: eleven single-player routes on the development build, three of
  them (item shop, weapon shop, storage) on the 64-bit build drawn 427
  columns wide; two players with the item shop and the weapon shop open.
- The song survey gives the same result on both builds.
- The Windows package with a window (144 Hz display, sound muted, ten
  seconds): 59.65 game frames a second, no picture left out, the sound
  reserve never empty; the launcher starts.
- The Android emulator (x86_64): the play-through gives the same hash and the
  same sound. With the picture, while the same PC was also running a virtual
  machine, the game ran at 46 frames a second, so that run says nothing about
  speed.
- From 0.7.4 and 0.7.5, unchanged code: outages of 3 to 75 seconds with two to
  four players (60 of 60 whole again), two outages in one session, a change
  of map with a player cut off.

## Not checked

- **A real network.** Every online test ran on one machine through a local
  relay that drops, holds or cuts packets. Sessions between real places were
  played by a few people with earlier versions, not measured.
- **By a person, with this version**: combat online, the music by ear, the
  transparencies in the scenes that were reported, a real controller, a
  phone, Linux with a window, signing in to RetroAchievements.
- The Linux x64 package of this version had not been run when the release was
  published (0.7.4 was, in a virtual machine); the result is added here when
  it is in.
- The rules for a gone player at level exits, portals, boss portals and the
  party's defeat: no scripted session reaches them yet.
- Fights, items and many enemies appearing during an outage.
- The music correction was never seen at work: no scripted session produces
  the case. Tell us where the music is missing.
- The launcher's Saves page: drawn in a test, its buttons not pressed.
- Joining a game in progress beyond the scripted first maps: fights, exits,
  bosses, two newcomers at once.

## Known problems

- A player the others gave up (15 seconds of silence) while they pass into the
  next map cannot come back into that session; its game is told the session
  is over.
- When player 1 (the host) leaves, the session ends for everybody: the game
  gives the host the enemies and the items.
- After the host's program has really ended, the others can only walk for up
  to a minute before the error appears.
- A sprite that starts to alternate blinks once or twice before it is drawn
  steadily.
- Enemies still go for a standing character whose player is gone.

## Install

- Windows: unpack `ShiningSource-0.8-windows-x64.zip` and start
  `ShiningSource.exe`. The launcher asks for your ROM the first time.
- Android: install `ShiningSource-0.8-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
- Linux x64: unpack `ShiningSource-0.8-linux-x64.tar.gz` and run
  `./ss2_linux64 your-rom.gba` (no launcher yet; `LINUX.txt` has the settings).

Reports are welcome as GitHub issues. A crash on Windows writes
`shiningsource_crash.log` beside the program, and an online session writes
`shiningsource_net.log`: attach them.
