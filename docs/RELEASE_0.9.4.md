# ShiningSource 0.9.4

- **Flat mode and GA36 (alpha).** Flat is the wide view's idea for square-ish
  screens such as 4:3: a 320 × 240 picture at a whole-number scale. In the
  field the map continues above, below and beside the console's picture with
  the game's own tiles and sprites, fading into black at the map's ends with
  rounded corners. The status display goes to the top edge, and the message
  window, the speaker's portrait and other interface at the console's bottom
  edge go to the bottom edge, so the field shows in between. Menus, shops and
  cut-scenes keep their original framing with black around it until 4:3
  artwork is approved. A new package for the GA36-MB V1.2 handheld (Allwinner
  A33, 640 × 480 panel) on goodluckOS and similar firmware uses Flat at exactly
  2×, filling the screen. The package brings its own libraries and writes
  straight to the framebuffer. An automatic diagnostic entry records the
  console's capabilities and measures performance in one run. Verified in an
  ARMv7 emulator with a 32-bit panel; the first console run of the previous
  build stopped at the save screen, which this version fixes.
- **Flat on PC (experimental).** Set `SS_CANVAS=320x240` to try Flat in a
  window. The usual picture is unchanged when it is not set.
- **ARM crash reports.** On Linux ARM a crash now writes
  `shiningsource_crash.log` with the registers and the code addresses, which
  `tools/crash_names.py --ga36` turns into function names.
- **Public lobby.** A room closes for new players as soon as a retransmitted
  input carries the start frame.
- **Android ARMv7 and GA36 coroutine stacks.** The game's coroutine stacks are
  mapped above the addresses the game reads as its own memory; on the GA36 the
  previous build lost them and stopped at the save screen.

## GA36 installation

Extract `ShiningSource-0.9.4-ga36-alpha.zip` into the `ports` folder of the
card, next to the other ports, and copy your own unmodified European
Shining Soul II dump to `shiningsourcega36/roms/rom.gba`. Open
**ShiningSource GA36** from Ports. Start + Select opens the options.
Saves, settings and `last-run.log` are in `shiningsourcega36/user/`.
