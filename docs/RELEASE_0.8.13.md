# ShiningSource 0.8.13 — narrow-map edges and menu compositing (Windows)

The fade at the edges of a narrow map now covers world sprites as well as scenery, including sprite pixels outside the map. The transition is eight game pixels wide instead of 32, reducing how far it darkens the original map.

Opening Start or another page keeps the faded world underneath it. UI backgrounds and HUD/page sprites retain their own colours. Translucent UI blends with the already faded world instead of darkening the menu itself. HUD sprites are identified at rasterization using their full bounds, so a world sprite crossing the HUD band is faded consistently. The game's brightness effects cannot relight world pixels beyond the map edge.

## Verification

- The ROM-free C compositor regression covers both map edges, priority-zero world sprites, pixels beyond the map, the eight-pixel limit, UI backgrounds and tagged sprites, transparent menu holes, translucent menus, brightness increase, and HUD sprite bounds.
- The existing text-row regressions pass (2 tests).
- Windows x32 game image compiled and linked. The compiler reports the existing unused `tile_pixel` helper warning.
- Scripted town replays exercise Start, inventory, status and window-colour pages at 240 and 427 pixels, and the item shop at 427 pixels. Their reports check completion, no reached stubs or crashes, menu/event entry and restored player control, inventory and money.
- The complete 11,286-frame classic menu replay produces the same picture hash as the original 0.8.12 renderer (`97664297`), compiled separately from the previous commit.

These checks cover compositor cases and scripted town routes; they are not a complete visual audit of every menu or map. No complete playthrough or new cross-platform runtime test is claimed.

## Delivery

This release supplies Windows x64. Android and Linux remain on 0.8.11. Online rules remain 7 and compatibility is unchanged. The official local achievements included in 0.8.12 remain included. The package uses the explicit file allowlist and verified SHA-256 manifest; no ROM, saves, settings, credentials or private logs are included.
