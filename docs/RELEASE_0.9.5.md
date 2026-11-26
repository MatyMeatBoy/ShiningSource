# ShiningSource 0.9.5

## Every platform

- **Online: the music no longer stops when a player falls** (issue #6). A
  player's defeat used to silence the area music on every other player's
  machine until they opened a menu. The cue that copied the defeat to the
  others was an empty song on the music player; it is gone, and no cue that
  comes from another player can touch the music player any more. The fallen
  player still hears their own defeat tune, as on the cartridge.
- **Damage numbers and popups stay with the world.** In the wide views and in
  Flat only the status display's own sprites go to the screen edges; damage
  numbers, item pickups, the enemies' "!" and effects no longer jump to the
  corners near the top of the console's picture.
- **No stutter when the scene changes.** Every scene's side and Flat art is
  read before the first picture (entering the forest took up to 228 ms).
- **Transitions.** Going through a door no longer shows two frames of the
  console's own layout, a dotted background or the outside's colours in the
  extended field.
- **Renderer on every core.** The picture is drawn in independent bands of rows
  and the blink smoothing in independent ranges, so every platform with worker
  threads uses all its cores; each row only decodes the sprites that reach it.
  Pictures are identical on every platform and view (checked frame by frame).
- **Stability.** The game's BIOS copies into its link queue (a null pointer in
  single player) are ignored, as the console ignores them. Reads through
  pointers that lead nowhere give zero on ARMv7 Linux and now on Android ARMv7
  too, as on PC and ARM64, instead of closing the game. Random-input stress
  test: 4 maps x 30,000 frames, no crash, memory stable.

## Flat (4:3)

- Every shop, the bank, Objects and Status have the status display at the top
  edge (2 px from it) and the shop dialogue at the bottom; Objects and Status
  fill the screen with their moving marble; shops use the approved 16:9 art
  fading to black; sequences without art (the intro book) sit on black with
  soft edges. NPCs just below the console's picture no longer appear at the top.

## GA36 (v0.1.6)

- Leaves alpha. Always Flat at exactly 2x on the 640 x 480 panel. The screen goes
  through DRM with three buffers, so the picture is never torn, and the panel is
  retimed from 55.13 Hz to 59.73 Hz by shortening its blanking: the panel paces
  the game and every frame is shown once (it falls back to 55 Hz by itself).
  About 85 frames per second of drawing capacity, 59.7 fps in play with 99.4 %
  of frames on time. The bottom face button is the GBA's A and the right one its
  B. The diagnostic package adds a "Medir FPS" entry.

## Installation

- **Windows:** extract the zip and run `ShiningSource.exe`.
- **Linux x64:** extract the tar.gz and see `LINUX.txt`.
- **Android:** install the APK and choose your ROM.
- **GA36:** extract the GA36 zip into the card's `ports` folder and copy your
  own unmodified European Shining Soul II dump to
  `shiningsourcega36/roms/rom.gba`. Start + Select opens the options.

No game data is included: every platform needs your own European
Shining Soul II (AU2P) dump.
