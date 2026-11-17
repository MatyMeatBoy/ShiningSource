# Edge fading and Windows 0.8.13

User requirements: fade the world and player at narrow map edges, keep the transition short, preserve every menu's colours while leaving the faded world visible underneath, and publish the next release with related improvements.

Design: apply edge attenuation to world colours before hardware blending with UI. Keep native hardware windows and brightness effects, exempt UI backgrounds and the existing HUD/page sprite classification. Use an eight-pixel transition instead of 32. Extra improvements cover sprites leaking beyond the edge and translucent menu blending. Windows is the delivery platform, matching 0.8.12; other platforms are not claimed rebuilt.

Plan (executed inline):
- [x] Extend the compositor regression with menu-open world pixels, UI sprites, transparent holes and translucent UI. Confirm failure before the correction.
- [x] Move world fading before UI blending; retain the short transition and outside-map sprite suppression. Run regression and review the diff.
- [x] Compile the Windows game image; run scripted rendering and existing relevant regressions using isolated save/output files.
- [x] Add release notes and README entry; package 0.8.13 with the explicit allowlist and verify archive hashes.
- [ ] Review the finished changes, commit, push and publish v0.8.13; verify release assets.

Verification limits: synthetic compositor checks do not establish every in-game menu layout. Record actual replay coverage before release.

Review: the final independent review found no release blocker. Brightness ordering and whole-sprite HUD classification were corrected. Page sprites still follow the renderer's existing priority-zero UI convention; no affected game state was demonstrated. Replays passed for menus at 240/427 and the item shop at 427. Classic replay hash matches 0.8.12 across 11,286 frames.
