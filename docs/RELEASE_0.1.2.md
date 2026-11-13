# ShiningSource 0.1.2

Works online with 0.1.1 (the rules of a session did not change). 0.1 does not
meet either.

## New

- The launcher wears the port's own look (the Soul field and gold frames of the
  classic mode's border) and shows the game's title screen. That picture is
  made on your machine from your ROM the first time and kept in the folder
  `cache` next to the launcher; no artwork of the game is in the download.
  F11 switches the launcher to full screen.
- Enhanced mode: where the picture has nothing to show (beside a menu, past the
  edge of a map) the Soul field is drawn instead of black.
- Achievements page: sign in with your RetroAchievements account and see the
  game's list for your ROM. The password goes only to retroachievements.org and
  is not kept; the session token stays on your PC. The game does not evaluate
  achievements while you play yet, and Rebel mode is not there yet.

## Checked since 0.1.1

- Saving: the game's own save routine was run in the first field and answered
  "saved and verified"; the save file holds the character's two sectors with
  valid checksums. Not yet tried by hand from the game's menu, nor after leaving
  an online session, which is where it failed in 0.1.

## Not checked

- Signing in to RetroAchievements (it needs an account).
- The Android package and the ARM64 game image were built but not run again
  with these changes.

Everything else is as in the 0.1.1 notes below.
