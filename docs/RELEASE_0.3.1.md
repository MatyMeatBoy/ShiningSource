# ShiningSource 0.3.1

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`).

Plays online with 0.3. Neither meets an older version.

## Since 0.3

- **When player 1 leaves an online session** the others' game starts again by
  itself and is back at the title, instead of closing. (The session itself
  cannot go on without player 1: the game gives player 1 the enemies and the
  items. Any other player who leaves, crashes or loses the connection stays as
  a standing character and the rest play on, as in 0.3.)
- **Browser build**: saves are kept between visits (in the browser's own
  storage), and it builds again with the new link code. Still built from
  source; no hosted page ships with this release.

Everything else is 0.3: see [RELEASE_0.3.md](RELEASE_0.3.md) for the rebuilt
online play, the wide view, real transparency, achievements and Rebel mode, the
Android start screen and the icon.

## Install

- Windows: unpack `ShiningSource-0.3.1-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.3.1-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives `5F1DB1A8` on Windows x64, as in
  0.3.
- Browser: a saved character is offered again after reloading the page.

## Not checked

- The restart after player 1 leaves, in a session with windows (the test runs
  have none and simply end there).
- This package on the Android emulator (0.3's was; the changes since do not
  touch Android's code paths).
- Everything listed as not checked for 0.3, first of all real sessions over
  the internet.

## Known

- A crash was reported when a player opened a shop in multiplayer (0.2). The
  cause has not been found yet; if it happens, `shiningsource_crash.log` next
  to the program says where.
