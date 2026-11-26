# ShiningSource 0.9.5

## Every platform

- **The desert worm boss can be beaten again.** In the Driazhek Desert the
  giant worm ran to the top right corner and froze with its body missing, so
  the dungeon could not be finished. Each body segment's setup takes the
  segment's number in a register the decompilation's C leaves out (the
  cartridge passes it by chance); the port now passes it. The body is drawn,
  the fight goes through all its phases and the portal to the next level
  appears.
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

- **64-bit Linux and Android hosts.** The ARM64 host serves the picture's
  pieces on a thread per core, each entering the game image on its own stack
  below 4 GiB. A service a host does not offer now reaches the image as
  "none" instead of as a jump to address 0: the multi-core renderer's first
  call would have hit it on Android (ARM64, x86-64) and Linux (ARM64, x86-64).

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

## R36S

- The R36S package (RK3326, 64-bit ArkOS / dArkOS and the like)
  now runs like the GA36: always Flat, 320x240 at exactly 2x on the 640x480
  panel, with the Flat art; the picture goes to the panel through DRM with
  three buffers (never torn), the panel's measured rate paces the game when it
  is near 59.73 Hz and the sound follows it; the picture is drawn on the four
  Cortex-A35 cores. It is built from this tree's ARM64 game image (no longer
  taken from a published APK) against glibc 2.29 symbols, so ArkOS's 2.30 runs
  it. `last-run.log` ends with the frame rate and frame times. Checked in an
  emulated R36S (4 x Cortex-A35, dArkOS root, Ubuntu 20.04's glibc): 3,000
  frames identical to PC and the same Flat, 240 and 320 pictures. Not yet run
  on a physical R36S.

## Installation

- **Windows:** extract the zip and run `ShiningSource.exe`.
- **Linux x64:** extract the tar.gz and see `LINUX.txt`.
- **Android:** install the APK and choose your ROM.
- **GA36:** extract the GA36 zip into the card's `ports` folder and copy your
  own unmodified European Shining Soul II dump to
  `shiningsourcega36/roms/rom.gba`. Start + Select opens the options.
- **R36S:** extract the R36S zip into the card's `ports` folder and copy your
  dump to `shiningsource/roms/rom.gba`; see the README inside.

No game data is included: every platform needs your own European
Shining Soul II (AU2P) dump.
