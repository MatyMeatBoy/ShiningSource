# ShiningSource 0.9.6

## Achievements on the handhelds and Linux

- **RetroAchievements on GA36, R36S and Linux x64**, as on Windows. The game's
  set (52 achievements) is evaluated by the game itself.
  - Every unlock shows its notice with its badge, with or without internet.
    The set, the badges and a certificate bundle ship in each package.
  - Without internet, unlocks wait in `retroachievements_pending.txt` and
    are sent (softcore) the next time the game starts online.
  - `retroachievements_estado.txt` shows the account, whether the game is
    connected and how many unlocks are waiting.
  - A Rebel run (cheats) never sends anything. `SS_RA_SUBMIT=0` keeps every
    unlock local.
- **Signing in**
  - **GA36:** if ReKHCoM is already signed in on the same card, its sign-in is
    used. Otherwise run `Vincular-RetroAchievements-ShiningSource.bat` from a
    PC once. The password is never kept.
  - **Linux:** `python3 ra_vincular_cuenta.py .` in the game folder, then
    start the game with `./shiningsource.sh`.
- **The start notice** (online, offline or local mode) shows the first time,
  and again only when the account syncs after a start without internet.
- **GA36: "Revisar Logros"** is a new Ports entry that checks what the account
  link needs in about a minute: clock, network, DNS, OpenSSL, and the account
  files by size only.
- **The ROM is hashed once at start**, not twice.

## Smoother handhelds (GA36, R36S)

- **The panel paces the game for the whole session.** Before, the game fell
  back to its own clock within half a second of play.
  - GA36 measured: 59.73 frames a second, 0.13 % of frames late.
- **Faster start.** The panel mode that reaches 59.7 Hz is remembered in
  `user/kms-mode.txt`.
  - GA36 measured: every start after the first shows its first picture in
    about 1.4 s instead of 2.7 s.
- **Steadier sound.** A panel given a new mode settles before it is measured.
  One reading of 59.30 Hz would have put the sound 0.7 % flat.

## Tests

- **Updated test harnesses:** the edge fade and language tests follow the
  banded renderer and the packed present call. The GA36 report test runs
  without the joystick capture that is not in the repository. The whole suite
  passes.

## Installation

- **Windows:** extract the zip and run `ShiningSource.exe`.
- **Linux x64:** extract the tar.gz and run `./shiningsource.sh your.gba`.
  See `LINUX.txt`.
- **Android:** install the APK and choose your ROM.
- **GA36:** extract the GA36 zip into the card's `ports` folder. Copy your
  dump to `shiningsourcega36/roms/rom.gba`.
- **R36S:** extract the R36S zip into the card's `ports` folder. Copy your
  dump to `shiningsource/roms/rom.gba`. The README inside has the details.

No game data is included: every platform needs your own European
Shining Soul II (AU2P) dump.
