# ShiningSource 0.9.4

- **GA36 (alpha).** A new package for the GA36-MB V1.2 handheld (Allwinner
  A33, 640 × 480 panel) on goodluckOS and similar firmware. The game draws a
  4:3 picture of 320 × 240 at exactly 2×, filling the screen. In the field the
  map continues above, below and beside the console's picture with the game's
  own tiles and sprites, and the status display sits at the top edge. Menus,
  shops and cut-scenes keep their original framing with black around it until
  4:3 artwork is approved. The package brings its own libraries and writes
  straight to the framebuffer. An automatic diagnostic entry records the
  console's capabilities and measures performance in one run. Verified in an
  ARMv7 emulator with a 32-bit panel; the first console run of the previous
  build stopped at the save screen, which this version fixes.
- **4:3 picture on PC (experimental).** Set `SS_CANVAS=320x240` to try the same
  4:3 view in a window. The usual picture is unchanged when it is not set.
- **ARM crash reports.** On Linux ARM a crash now writes
  `shiningsource_crash.log` with the registers and the code addresses, which
  `tools/crash_names.py --ga36` turns into function names.
- **Public lobby.** A room closes for new players as soon as a retransmitted
  input carries the start frame.
- **R36S.** The ARM64 Ports package from 0.9.3 is built from this version too.

## GA36 installation

Extract `ShiningSource-0.9.4-ga36-alpha.zip` into the `ports` folder of the
card, next to the other ports, and copy your own unmodified European
Shining Soul II dump to `shiningsourcega36/roms/rom.gba`. Open
**ShiningSource GA36** from Ports. Start + Select opens the options.
Saves, settings and `last-run.log` are in `shiningsourcega36/user/`.
