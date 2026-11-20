# ShiningSource 0.9.3

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
- **Restored Multiplayer Audio.** Successful pickups are heard by nearby
  teammates in the same area. A failed pickup or a repeated synchronization
  message does not replay the sound. Two boss impacts that previously played
  only for the player hit are now shared in the same area. The original door
  and map transition sounds continue to use the game's shared actions.

When an established connection drops, keep the game open so it can catch up.
A new game process after a crash cannot yet restore that character in an
ongoing dungeon.
