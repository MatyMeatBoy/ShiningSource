# ShiningSource 0.9.5 (draft)

- **GA36 v0.1.** The GA36-MB V1.2 package leaves alpha. It always runs in Flat,
  the 4:3 composition: the field beyond the console's picture on every side,
  the status display at the top edge, the message window and the speaker's
  portrait at the bottom edge. The picture is now drawn on the A33's four
  cores, and the copy to the screen no longer reads video memory back. The
  bottom face button is the GBA's A and the right one its B. A short automatic
  "Medir FPS" entry measures the frame rate stage by stage.
- **Flat art.** Language, title, save, character creation and shops are framed
  on all four sides in Flat with art grown from the approved 16:9 art (every
  pixel is a pixel of that art; drafts pending approval). The earlier
  ultrawide atlases were never approved and are removed: ultrawide shows the
  16:9 art.
- **Crash fixes on ARM.** The game's BIOS copies into its link queue, which is a
  null pointer in single player, are ignored as the console ignores them; reads
  through pointers that lead nowhere give zero on the ARMv7 build as they do
  on PC. Both stopped the GA36 build (the apple scene, a skipped scene). A crash
  on Linux ARM writes `shiningsource_crash.log`.
- **Renderer.** The renderer draws the usual frame in independent bands of rows
  and the blink smoothing in independent ranges, so platforms with worker
  threads use every core; each row only decodes the sprites that reach it. The
  pictures are identical on every platform and view (checked frame by frame).
- **GA36 v0.1.6** (published first as the prerelease v0.1.6-ga36). The screen
  goes through DRM with three buffers, so the picture is never torn, and the
  panel is retimed from 55.13 Hz to 59.73 Hz by shortening its blanking: the
  panel paces the game and every frame is shown once (it falls back to 55 Hz
  by itself). Scene art is loaded at start, so changing scene no longer
  stutters. In Flat, doors no longer show two frames of the console's layout,
  NPCs below the picture no longer appear at the top, and damage numbers,
  pickups, the enemies' "!" and effects stay with the world: only the status
  display goes to the edges. Every shop, the bank, Objects and Status have the
  status display at the top (2 px from the edge) and the shop dialogue at the
  bottom; Objects/Status fill the screen with their moving marble; shops use
  the approved 16:9 art fading to black; sequences without art (the intro
  book) sit on black with soft edges. Verified on the console by the user,
  in QEMU (fb and DRM) and by 4 x 30,000 frames of random play on PC.
