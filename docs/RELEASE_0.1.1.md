# ShiningSource 0.1.1

Fixes from the first two-player test of 0.1. Everyone needs this version to
play together: 0.1 and 0.1.1 do not meet online.

## Fixed

- **Saving did not work at all** ("save error", alone or online). The port
  presented its save chip to the game with the identifier at the wrong place,
  so the game refused to save. The game now accepts it. Not confirmed yet with a
  save made from the game's own menu by the author: please check it first.
- Online delay is automatic: a session starts at 2 frames (the cable's own is
  1) and the host raises it only when the connection needs it (measured: 2
  frames with no lag, 4 with 45-60 ms each way, 10 with 110-140 ms). This is
  what made the other player and the enemies look late. A fixed delay can
  still be set in the Online page.
- Wide view: menus drawn over the field (status, items, L/R, text boxes) came
  apart near the edges of a map, and the status screen could show field tiles.
  The console's picture now always stays in the middle, the top bar is not
  divided, and a screen that is not the field keeps the original picture. Past
  the edge of a map the view is black, not the backdrop colour.
- Controller: the bottom face button (Cross / A) could not be assigned. The
  Controls page shows the names printed on the connected controller.
- Sound: samples are interpolated and loud passages are limited softly instead
  of being cut flat.

## New

- Skip scene: Start during a story scene asks whether to skip it; the game then
  runs ahead without picture or sound and fades back in. Online, Start is a
  vote and the scene is skipped when every player has pressed it, all at the
  same frame. Can be turned off in Video.
- Synchronisation in Video: VSync (default; the display paces the game at 60 Hz
  and multiples), VRR for G-Sync / FreeSync displays, or off.
- Two screen modes, asked on first start: Classic (the console's picture at a
  whole scale inside an adjustable border, with the port's own "Soul" frame)
  and Enhanced (adapts to the window).

## Still open

- Hits that seem to land from a distance online: a shorter delay reduces it;
  the game shows other players one delay behind by design. Prediction with
  rollback is not in this version.
- A launcher in the style of the game's menus, RetroAchievements login and
  achievements (normal list and Rebel mode).
- Everything listed under "Known limits" in the 0.1 notes.
