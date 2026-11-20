# ShiningSource 0.9 — RecompLauncher and combat fixes

- **Archer.** An arrow cancelled before its launch frame now releases its
  projectile slot. A cancelled attack could leave a slot occupied and eventually
  prevent ordinary and charged shots from spawning arrows.
- **Online co-op.** An enemy that receives lethal damage while waiting for its
  palette now applies that damage when initialization finishes. This closes a
  path that could leave a defeated enemy visible to one player.
- **RecompLauncher on Windows and Linux.** Easy Setup takes your own unmodified
  European Shining Soul II `.gba` (AU2P) and verifies it before Play. The game's
  page exposes picture mode, fullscreen, display sync, volume, sprite flicker
  smoothing and restored multiplayer audio. Existing desktop launch methods
  remain available.

Online rules remain 7. Android uses the same app ID and signing certificate as
0.8.14. When upgrading a manual installation, extract the complete archive so
the executable and `ss2_game.elf` come from the same release. The packages
contain no ROMs, saves or personal settings.

## Verification

The regression tests cover cancelled arrows and lethal hits during enemy
initialization. A 30,000-frame run with three Windows clients ended with the
same enemy defeated on every client. The exact intermittent player reports
remain unconfirmed. A 32,000-frame offline replay matched the 0.8.14 Windows
build in video hashes and audio statistics.

The graphics check covered item descriptions from inventory, equipment and a
shop in the classic and widescreen views on Windows. Descriptions that showed
corrupt glyphs in 0.8.10 were readable with the current renderer.

RecompLauncher setup was checked through the launch commands on Windows and
Linux, including invalid ROM handling, and against the SDK schema. Testing the
full flow through the RecompLauncher app is still pending.
