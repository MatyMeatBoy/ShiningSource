# ShiningSource 0.7.4

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Online, 0.7.4 meets 0.7 to 0.7.3 (nothing in the packets changed), but only a
player with 0.7.4 gets what is new here, and a host with an older version
takes nobody in again. **Everybody should update, the host first.**

## Online: a connection that drops comes back

Measured first, with a relay that cuts one player off for a while
(`tools/net_outage_test.sh`):

- **Before**, any silence longer than two thirds of a second, even one far
  below the 15-second limit, left that player frozen on the others' screens,
  and them on its own, for the rest of the session, although the game's events
  went on. After 15 seconds the two sides had given each other up for good.
- **Now** a short gap heals by itself, and a player the others had given up is
  taken in again when its packets return. It is sent every event it missed, in
  order and once each (enemies, items, changes), while the others play on
  without waiting.
- A player who hears nothing at all from the server knows it is its own
  connection and gives nobody up; it waits up to two minutes. The window title
  says so.
- The host is waited for a minute instead of 15 seconds. The price: when the
  host's program has really ended, the others stand in a world where only
  walking works for a minute before the error appears.
- If the events cannot be replayed (too many were missed, or the host runs an
  older version), the returning player is told the session is over instead of
  being let in with a world that differs.

## Also

- The experimental LT / RT option can be switched in the game's options menu
  (Escape, or Select + Start), not only in the launcher.

## Checked

- Two and three players on one machine, a player or the host cut off for 3,
  10 and 30 seconds, five runs each: 60 of 60 sessions whole again (same
  events in the same order on every player, nothing frozen, nobody left out).
  Before the change: 0 of 36.
- The same with a bad network (60 to 80 ms each way, 5% of packets lost): 18
  of 18. A player away 75 seconds: 4 of 4. Everybody cut at once: 4 of 4. The
  64-bit Windows build: 3 of 3.
- A returning player was 169 to 262 events behind and had them in 0.08 to
  0.14 seconds (0.5 to 1.0 on the bad network).
- The released source, built again: a player away 30 seconds and a host away
  10 seconds heal; the scripted 32,000-frame play-through gives `5F1DB1A8` and
  the same sound on the development build and Windows x64.

- After the release, from the same source: the play-through gives the same
  hash and the same sound in the Android emulator (x86_64). The first attempt,
  started while the emulator was still booting, ended at frame 28,380 without
  a report or a crash record; the repeat ran through.
- After the release, on Linux x64 and Linux ARM64 (Debian 12 virtual machines,
  no window): the play-through gives the same hash and the same sound, a
  two-player session through a relay built there runs to its end, and a player
  cut off for 30 seconds is taken in again (one run each).

## Not checked

- **A real network.** The outage is the relay not forwarding, on one machine.
  A connection that returns with another address (Wi-Fi to mobile data), a
  phone, a suspended laptop: not tried.
- A change of map while a player is away, and fights during an outage: the
  scripted session only walks in the first field.
- Four players, two outages in one session.
- **Linux with a window.** The Linux x64 package added to this release was
  only ever run without one: picture, sound and a controller on a real desktop
  are untried.
- An online session on Android with this version.
- Everything listed as not checked in 0.7.3 and before.

## Install

- Windows: unpack `ShiningSource-0.7.4-windows-x64.zip` and start
  `ShiningSource.exe`.
- Linux x64: unpack `ShiningSource-0.7.4-linux-x64.tar.gz` and run
  `./ss2_linux64 your-rom.gba` (no launcher yet; `LINUX.txt` has the settings).
- Android: install `ShiningSource-0.7.4-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).
