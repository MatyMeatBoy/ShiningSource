# ShiningSource 0.8.4

Shining Soul II (Europe) as a native program for Windows x64, Android and
Linux x64. This version carries everything done since 0.8.1. (There is no
0.8.2 or 0.8.3.) What the port is and does is in the
[0.8 notes](RELEASE_0.8.md).

No ROM, BIOS or game data is included: you supply your own cartridge dump
(SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). Online, play 0.8.4 with
0.8.4: nothing in the packets changed, but what is new is decided by each
player's program.

## Online

- **A dungeon level, tested by script.** Three scripted players enter the
  first dungeon, fight (enemies fall, items drop and are taken) and pass the
  level's exit together (`tools/net_exit_test.sh`). Until now the scripted
  sessions only walked. With it:
  - the others pass the exit after a player's program was ended in the fight
    (the rule 0.8 applied from reading the code);
  - a player cut off for 30 seconds during fights is taken in again, with the
    same enemies and the same events in the same order on every player.
- **Enemies leave a departed player's character alone.** They used to go for
  it and stand beside it for the rest of the level (an enemy beside it at 72
  of 74 samples). A player that left for good is now handed to the game as a
  defeated one, which the enemies pass over. A player that may still return
  (given up for silence) stands as before until the party passes into the
  next map: marking it at once would leave it "defeated" on the others'
  screens after it came back.

## Picture

- **Experimental: a larger status display.** Off by default ("Experimental:
  larger status" in the game's options menu, a checkbox on the launcher's
  Video page; enhanced mode only). On a large window the field is drawn large
  and the HP, SP and Soul bars and the L and R menus stay small beside it.
  With the option on they are drawn with larger pixels, sharp, at their
  corners: 1.5 to 1.8 times the field's on a 16:9 window, twice on a wider
  one. If the layer cannot be drawn, the picture stays whole and the option
  switches itself off for that run. With the option off the picture is the
  same as before, pixel for pixel.
- One boss scene's line handler waits for a display flag nothing raised; it
  is raised now. That scene was not reached in a test.

## Launcher (Windows)

- The Saves page's buttons were pressed in a test (0.8.1 had this already).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` and the same sound
  on the development build and Windows x64, from the packaged files too; the
  picture hashes at 240, 284, 427 and 480 columns are those of 0.7.1 with the
  status display option off.
- The dungeon level, three players, one run each: nobody gone; a program
  ended in the fight; a 30-second outage during fights.
- Regressions on the released source: three players with one program ended in
  the first field (the others go on), three players with a player cut off 30
  seconds (whole again), a newcomer joining two players (joined).
- The status display option on, in a window (sound muted): the game keeps its
  speed, no picture left out; the layer was inspected in dumps and window
  shots of the first field, the town's hall, a message, the status page and
  the L and R menus.
- After the release, the Linux x64 package as published, in a Debian 12
  virtual machine (no window): the play-through gives the same hash and the
  same sound, a two-player session runs to its end, and a player cut off for
  30 seconds is taken in again.
- After the release, the Android emulator (x86_64): the play-through gives
  the same hash and the same sound, twice. A first attempt right after the
  emulator had started left no result, as with 0.7.4; with the status display
  option on and the picture shown, the game ran at 58 frames a second on that
  slow device, leaving pictures out.

## Not checked

- **The larger status display by a person**: on a real display, in full
  screen, at other window sizes; its menu entry and launcher checkbox were not
  clicked in a test. That is why it is experimental.
- **The party's defeat online with a player gone**: in one scripted run, with
  one seat gone and the two others down, the level did not end. There is no
  run of the same without a gone seat to compare with, so it is not known
  whether the gone seat is the cause. Listed as a known problem.
- A player that quits cleanly in the dungeon (one run ended before it could
  tell), a 10-second outage in the dungeon, what a departed character looks
  like on screen once it counts as defeated.
- Everything listed as not checked in 0.8.

## Known problems

- Those of 0.8, and: with a player gone and everybody else defeated, the
  level may not end (see above).

## Install

- Windows: unpack `ShiningSource-0.8.4-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.8.4-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
- Linux x64: unpack `ShiningSource-0.8.4-linux-x64.tar.gz` and run
  `./ss2_linux64 your-rom.gba` (no launcher yet; `LINUX.txt` has the settings).
