# ShiningSource 0.9.2

## Launcher and controls

- The Windows launcher has an integrated title bar, rounded menus, textured
  buttons and the Shining Soul II cover from the publisher's game page. The
  cartridge preview uses the player's own ROM.
- Navigate with a controller or keyboard. The controller can browse local
  folders for a ROM and start the game. Text fields accept keyboard input.
- Android shares the cover, textured controls and widescreen title and menu
  artwork. Its launcher has Home, Video, Online and Settings tabs. Existing
  ROM copies are detected. A previously selected document can restore a
  missing copy when Android still grants access to it.
- Choose **Borderless**, **Windowed** or **Exclusive fullscreen** for the game.
  Borderless is the default for new configurations. Existing fullscreen
  settings are recognized.
- The GameCube profile uses the physical A and B buttons, Z for Select, the
  L/R trigger click, the D-pad and the left stick. Standard mappings remain
  separate. Windows users with the official WUP-028 adapter need a compatible
  WinUSB driver. See the controller section in the README for setup.

## Picture and notices

- **Wide Pro** in Enhanced mode shows the original 240 × 160 center at the
  same integer scale. It extends supported title screens, Objects, Status and
  shop menus into the two outside regions. The SEGA logo keeps its original
  fade. Wide Pro can be disabled in Video.
- Achievement badges live in `achievements/badges/`. Existing loose badge
  files are migrated, and existing achievement records and credentials are
  preserved.
- A mode notice appears once. Local mode explains that achievements are kept
  on this PC. Achievement unlocks keep their own notices.
- The game remembers the language confirmed in its original first-start
  selector. The scene-skip notice and Start + Select menu use that language.
  Change the game's language in the options menu and reopen the game to apply
  it. The Windows launcher defaults to the Windows display language when it
  supports it, with English as the fallback.

## Co-op

- From the game's title, choose **Multiplayer** to create a room or enter
  an eight-character code. The default is the ShiningSource Server. A custom
  IP or a preselected room remains available in the Online tab. By default,
  starting the game or playing solo leaves the multiplayer connection idle.
- Game Link displays each player's connection, character setup and ready
  state. Confirmed character classes and palettes use the cartridge sprites.
  Start retains the original host and player-readiness rules.
- Scene-skip votes appear above the dialogue. In rooms where everyone uses
  the updated protocol extension, each player can start one vote per scene.
  An attempt expires after 30 seconds and requires a yes from everyone.
  Repeating Start to cancel spends the initiator's attempt. Another player can
  open a fresh vote, and everyone can vote on that new attempt.
- Weapon selection passes the item's real subtype to the original skill-rank
  check. This fixes a port call that could apply the wrong rank requirement.
- When the port rebuilds a local player at a scene change, it restores the
  active equipment slot if the original initialization left the actor unarmed.
  This keeps a new character's starter weapon ready, including in co-op.
- Restored Multiplayer Audio is on by default. It includes compatible
  companion attack and impact cues during online play.
- A temporary outage can reconnect an existing running client to its seat.
  Rejoining after a full process restart and joining an active town session
  are still pending. Existing sessions reject new admissions once play starts.

## Upgrading

Extract the complete Windows or Linux archive together so the launcher,
host, game image and assets come from the same version. Back up your save
before replacing a manual installation. The Android APK updates in place
with the same app ID and signing certificate.

## Artwork

The launcher cover comes from the [publisher's Shining Soul II page](https://atlus.com/atlus-titles/ss2/).
The new side artwork is drawn outside the cartridge picture. Packages require
the player's own European ROM and include no ROMs or saves.
