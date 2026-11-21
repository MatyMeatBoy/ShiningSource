# ShiningSource 0.9.3

- **Wide Pro.** Enhanced mode adds matching 16:9 side art to save selection.
  Items and Status share an extended background that scrolls and fades with the
  original menu texture. At
  ultrawide ratios the publisher screen, language selection, title, save
  selection, character setup, and merchant screens have additional side
  textures. The original 240 × 160 image stays centered.
- **Public lobby.** Choose Multiplayer at the title to create a room or select
  one that is already open. The list refreshes automatically and shows available
  seats. Use the game-style keyboard to enter a title, or accept the next
  available numbered ShiningSource title by pressing Start. A server that
  requires a newer version displays an update message. Once the host starts,
  new players can join another open room.
- **Direct connections.** The Windows launcher checks custom addresses and
  ports before launching. Hostnames such as `localhost` resolve through the
  relay's IPv4 path.
- **Android.** The game opens in landscape by default. Turn off **Play in
  landscape** in the launcher's Video tab to follow the device's rotation
  setting.
- **Launcher updates.** Check from About on Windows or Settings on Android.
  The launcher downloads the stable release, verifies it and asks before
  installing. Android uses the system installer.
- **Restored Multiplayer Audio.** Successful pickups are heard by nearby
  teammates in the same area. A failed pickup or a repeated synchronization
  message does not replay the sound. Two boss impacts that previously played
  only for the player hit are now shared in the same area. The original door
  and map transition sounds continue to use the game's shared actions.

## Local multiplayer on Windows

Choose **Local multiplayer** on the launcher Home tab. Press Select three
times on an unassigned controller to add its player. Each player chooses
a separate save profile and then chooses a character from the game's save
screen. Views share the largest common integer scale, adjusting as players
join. Enhanced is the default. The window adapts to the desktop using 1080p
16:9 as its reference and also supports 4:3. A textured background fills
the space outside each view. Choose Local for a private relay on this PC,
or Online to select the same open room from each view. The restored online
effects are off in both shared-screen modes.

You can add a new seat while waiting for the match to start. A previously
assigned controller can reconnect during play.

When an established connection drops, keep the game open so it can catch up.
A new game process after a crash cannot yet restore that character in an
ongoing dungeon.
