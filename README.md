# ShiningSource

Native port of the Game Boy Advance *Shining Soul* games, built from their
matching decompilations instead of from emulation or static recompilation.

| Game | Source | Status |
|---|---|---|
| **Shining Soul II** (Europe, `AU2P`) | [SS2Decomp](https://github.com/MatyMeatBoy/SS2Decomp): 100% matching C, named, data described | **In development** (see below) |
| **Shining Soul** | no decompilation yet | **Closed** until its decomp exists ([games/ss1](games/ss1/README.md)) |

No game code, ROM or asset is stored here. You bring your own cartridge dump;
the decompilation is a git submodule and the game data is read from your ROM.

## Where it stands

The native executable **runs the game and draws it**. Start-up, the task
scheduler, interrupts, saving and the picture are the port's own code; the game
logic is the decompilation, compiled unchanged. Today it goes from power-on
through language selection, title, character selection and naming **into the
first field scene**, with the HUD, the player, an NPC and an enemy on screen, in
a window with keyboard input and correct speed. There is no sound yet.

```
$ python3 tools/build.py compile
ss2 / dev32: 43 of 43 game modules compile, 0 error(s); 16 module(s) replaced by the runtime
$ python3 tools/link.py
build/dev32/ss2/ss2_dev32.exe: 716 absolute symbols (memory and ROM), 26 functions still stubbed
$ build/dev32/ss2/ss2_dev32.exe your_rom.gba          # opens a window
```

Keys: arrows, `X` = A, `Z` = B, `C` = L, `V` = R, `Enter` = Start, right
`Shift` = Select, `Esc` quits. `tools/run.sh FRAMES INPUT DUMP` runs without a
window with scripted input and writes the picture of one frame, which is how the
port is tested. No screenshots are stored here: they are game artwork.

Still stubbed: the sound engine (22 entry points) and the link-cable routines.
The order of work is in [docs/ROADMAP.md](docs/ROADMAP.md).

## What this port will be

- **The original game logic, natively.** Every function of the cartridge is C
  from the decompilation; the port adds a platform layer under it (video,
  audio, input, saves, link cable) and nothing in between.
- **64-bit** on PC, Linux and Android. Consoles use what their hardware allows.
- **Widescreen that is real**, not stretched: the field shows more of the
  authored map; menus keep their composition; the HUD follows the window edges
  and scales with it.
- **Online for 2-4 players** with crossplay between Windows, Linux and Android,
  on the same server infrastructure as
  [ReKHCoM](https://github.com/MatyMeatBoy/ReKHCoM), following the method
  documented in [GBANoCable](https://github.com/MatyMeatBoy/GBANoCable).
- Launcher with ROM verification, controller mapping, languages and
  achievements.

The design decisions and their reasons: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Building (developers)

Requirements: Python 3, git, and [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(set `SHININGSOURCE_LLVM_MINGW` to its folder).

```sh
git clone --recurse-submodules https://github.com/MatyMeatBoy/ShiningSource
cd ShiningSource
python3 tools/build.py compile      # milestone 0: every game module compiles
python3 tools/link.py               # milestone 1: links; lists what is still stubbed
```

`tools/build.py prepare` shows what is changed in the game's C for a non-ARM
compiler: the decompilation itself is never edited from this repository.

## Layout

| Path | Content |
|---|---|
| `games/ss2/decomp` | submodule: the Shining Soul II decompilation |
| `games/ss2/port` | what this game needs to build natively: modules the runtime replaces, source patches with their reason |
| `games/ss1` | closed placeholder |
| `runtime/` | platform layer shared by both games (memory map, BIOS, video, audio, input, saves, link) |
| `tools/` | build driver and generators |
| `docs/` | architecture, roadmap, porting journal |

## Español

ShiningSource es el port nativo de Shining Soul II (y, cuando exista su decomp,
de Shining Soul 1) hecho desde la decompilación: el juego original compilado
para PC, Linux, Android y consolas, con pantalla ancha real, HUD que se ajusta
a la ventana y online de 2 a 4 jugadores con crossplay. No incluye ROM ni datos
del juego. Hoy **el juego corre nativo y se ve**: arranca, pasa idioma, título,
selección y nombre del personaje y **entra a la primera escena de campo**, con
HUD, jugador, NPC y enemigo en pantalla, con ventana, teclado y velocidad
correcta. Aún no tiene sonido ni online. El plan está en [docs/ROADMAP.md](docs/ROADMAP.md).

## License

The port code in this repository is MIT ([LICENSE](LICENSE)). The games belong
to their owners; this project distributes none of their code or data.
