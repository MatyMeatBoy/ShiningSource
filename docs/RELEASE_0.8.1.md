# ShiningSource 0.8.1

A correction to 0.8, the first public release. Everything in the
[0.8 notes](RELEASE_0.8.md) holds; this version changes two things.

No ROM, BIOS or game data is included: you supply your own cartridge dump
(SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). Online, play 0.8.1 with
0.8.1.

## Fixed

- **A player returning after a long outage could be written off at once.**
  When the others gave a silent player up (15 seconds), a comparison of two
  clock readings taken a millisecond apart could come out as "away for 49
  days": that player was then marked as not expected back, and on its return
  it was told the session was over instead of being taken in again. It
  happened every time on Linux (found by running the 0.8 package there) and
  depended on timing elsewhere. In 0.7.5 and 0.8; not in 0.7.4.
- The launcher's Saves page: its buttons were listed as not pressed in a test.
  They are now: a copy is kept, and after the save changed the copy is put
  back with the changed save kept as another copy.

## Checked

- Two players with a player cut off for 30 seconds, three runs, and three
  players on the 64-bit build, one run: whole again every time.
- The scripted 32,000-frame play-through gives `5F1DB1A8` and the same sound
  on the development build and Windows x64.
- Before this correction, the 0.8 Linux package in Debian 12 virtual machines
  (x64 the published file, ARM64 built from the same source): the
  play-through gives the same hash and the same sound and a two-player
  session runs to its end on both; the outage case failed on both, which is
  how the fault was found. With the corrected line the outage case was run on
  ARM64 there: whole again.

## Not checked

- The 0.8.1 Linux x64 package itself had not been run when this was
  published; the result is added here when it is in.
- Everything listed as not checked in 0.8.

## Install

- Windows: unpack `ShiningSource-0.8.1-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.8.1-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
- Linux x64: unpack `ShiningSource-0.8.1-linux-x64.tar.gz` and run
  `./ss2_linux64 your-rom.gba` (no launcher yet; `LINUX.txt` has the settings).
