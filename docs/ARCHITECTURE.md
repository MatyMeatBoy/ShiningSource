# Architecture and decisions

Each decision states what was chosen, why, and what it costs. They come from
three sources: measurements on the Shining Soul II decompilation, the design
that already ships in ReKHCoM (the sibling port of Kingdom Hearts: Chain of
Memories), and the netplay method written down in GBANoCable.

## 1. The game is compiled, not rewritten

The decompilation is 130,000 lines of C that reproduce the cartridge exactly.
It is compiled as it is. The port never forks game logic: when the game needs a
different behaviour on PC (wider view, HUD placement, online), the change is a
hook with a name and a reason, kept in `games/<game>/port/`.

Measured on 2026-11-12: with five mechanical source transformations and three
one-line patches, 41 of 47 game modules compile with clang for x86.

Why not a game engine (Unity, Godot, Unreal): the cartridge is its own engine.
Its logic depends on exact frames, on its cooperative task scheduler and on its
memory layout. Re-authoring it would discard the decompilation.

## 2. Pointers stay 32-bit; the process is 64-bit

The game's C stores pointers in 32-bit integers everywhere (function results,
struct fields, 1,500 RAM symbols at fixed GBA addresses, pointer tables inside
the ROM data). Converting that to real 64-bit pointers would mean rewriting most
of it and would break the link to the matching decompilation.

ReKHCoM solved the same problem and ShiningSource adopts its three modes:

| Mode | Where | How |
|---|---|---|
| **host64 + guest** | Windows x64, Linux x64/arm64, Android arm64, PS5 | The game is compiled with 32-bit pointers for a 64-bit CPU (x32 / arm64_32) and linked as a flat image at a low address. A 64-bit host process maps the GBA address space at its real addresses and gives the game a small table of services (video, audio, files, network). 64-bit registers and instructions, 32-bit pointers. |
| **dev32** | developers | A 32-bit Windows process where GBA addresses are real addresses. Simplest to debug; everything proved here carries to host64. Not shipped. |
| **reloc** | Switch, Vita, anything that cannot map low addresses | GBA regions are arrays and pointer words are relocated at load. Slower to bring up; done last. |
| **wasm32** | web | 32-bit by nature; GBA addresses live in linear memory. |

How the low addresses are obtained (learned on 2026-11-12): asking the system
for fixed addresses at run time fails often, because Windows may already have
mapped something there. Both the dev32 program and the 64-bit host are
therefore linked at a low base and carry the address space the game needs as
one zero-filled block of their own image; a program's image is mapped before
anything else, so the block is guaranteed.

Cost: the game cannot address more than 4 GiB (it needs 300 KiB of RAM), and
the host/guest boundary needs a typed call table. Benefit: zero changes to game
logic for 64-bit.

## 3. The ROM is the data

The user's ROM is mapped at `0x08000000`, where the cartridge had it. Every data
pointer in the game stays valid, so no asset is extracted, converted or shipped.

The exception is pointers to *code*: tables in the ROM hold addresses of
functions that are now native. `tools/` will generate the list of those words
(a word in a described, non-graphic object whose value is a function entry) and
the runtime rewrites them at load to the native functions. The decompilation's
data description (SS2Decomp `tools/config/data_types.txt`, `data_arrays.txt`)
is what makes that list safe: graphics, samples and text are excluded by type.

## 4. Runtime: what replaces the hardware

| GBA part | Replacement | Notes |
|---|---|---|
| CPU start-up, interrupts | `runtime` frame loop: the game's own `AgbMain` runs; `VBlankIntrWait` hands the frame to the runtime, which draws, raises the interrupts the game enabled, presents and paces | same shape as ReKHCoM `PortFrame` |
| Task scheduler (hand-written ARM context switch) | native coroutines with the same entry points | the one piece of game-specific assembly to re-create; every `Task...` function yields through it |
| BIOS calls | C implementations with the documented semantics | `Div`, `Sqrt`, `ArcTan2`, `CpuSet`, `CpuFastSet`, `LZ77UnComp*`, `ObjAffineSet`, ... |
| PPU | software renderer into a buffer wider than 240x160; the GPU only presents | exactness first; widescreen and HUD layers are renderer features |
| Sound | native MusicPlayer2000 (m4a) sequencer and mixer reading the songs from the ROM | 12 decomp modules that mirror the hand-tuned sound assembly are replaced by this |
| Flash save | file with atomic replace | 64 KiB, same layout as the cartridge |
| Link cable | packet-level transport under the game's own multiplayer driver | see 7 |
| Code copied to RAM | redirected to the native function | the cartridge copies four routines to IWRAM for speed |

Platform services use **SDL3** (window, input, audio device, GPU presentation):
one API for Windows, Linux, Android and the consoles that have an SDL port.

## 5. Video: wide view and a HUD that follows the window

Facts taken from the decompilation and from the research documented for the
static recompilation of the same game:

- Wide view applies to field scenes: the engine keeps a full scroll position
  and maps larger than the screen (maps are 30 to 120 cells wide), so the
  renderer can draw real map columns to the left and right. Width goes from
  240 up to 480 pixels; 16:9 is 284x160.
- Menus, dialogue, cinematics and loading keep the native 240x160 composition,
  centred.
- HUD elements are sprites the game positions against the 240-pixel screen. The
  renderer places them in a separate layer, each kept at its distance from the
  screen edge it belongs to, and scales that layer with the window.
- Scene-change blackouts use window masks that must cover the extra columns.
- Sprite X positions 256-304 are valid on a wide view and must not wrap.

Because this port owns the renderer and reads the game's own structures
(named in the decompilation), these are implemented where they belong: in the
camera and draw-list code paths, not by pattern-matching video memory.

## 6. Frame pacing

The game is frame-locked at 59.7275 Hz. The runtime paces presentation to that
rate, with the display-sync and audio-clock strategies that were tuned for
ReKHCoM as the starting point. Game speed never depends on the monitor.

## 7. Online and crossplay

The model is the one in GBANoCable, already running for ReKHCoM:

- The cable is replaced **under the game's packet queue**. The game's own
  multiplayer driver (`MultiSio`: 32-byte packets carrying 24-byte blocks,
  2-4 players) keeps working; only its transport changes.
- One packet per player per frame, played with a fixed delay chosen once by the
  host from the measured round-trip time. Online, the cable never times out:
  the game waits.
- All platforms run the same integer-only game code, so a session between
  Windows, Linux and Android is deterministic by construction. A state hash
  every N frames detects divergence.
- Rooms are served by a relay process on the same VPS as ReKHCoM. Following the
  relay's own rule, Shining Soul II gets **its own process, port and protocol
  version**; it never shares the ReKHCoM namespace. Direct UDP and LAN use the
  same client code.

Requirement: up to four players in different parts of the world, without
dropped sessions. Fixed delay alone is not enough for that (a 300 ms round trip
would be felt on every button press), so the plan has two layers: the
GBANoCable slot model as the correct base, and, on top, prediction of remote
packets with rollback of the game state. Rollback needs small snapshots, which
this port can take because it knows the game's memory layout (300 KiB of RAM)
instead of saving a whole emulated machine. Reconnection uses a per-seat token
and a snapshot from the host.

Online ships on Windows, Linux and Android first.

## 8. Launcher, achievements, saves

- Launcher: the Dear ImGui + SDL3 launcher design of ReKHCoM (ROM check by
  SHA-1, settings file, controller probing and remapping, languages `en`/`es`
  first, online lobby). It is also the first screen on Android.
- Achievements: rcheevos, with the same policy as ReKHCoM (the port must say
  honestly what it is to the service; local port achievements are separate).
- Saves: the cartridge's own save format in a file, so saves move between this
  port, emulators and the cartridge.

## 9. Two games, one runtime

`runtime/` knows nothing about a specific game. `games/ss2/port/` holds what is
particular to Shining Soul II. Shining Soul 1 will get `games/ss1/` when its
decompilation exists; the two share the engine family, so most of the runtime
should carry over.

## 10. What stays out

- No ROM, BIOS, extracted asset or decompiled game source in this repository.
- No code from the static recompilation of the same game: it is used only as a
  record of which improvements matter and of the game facts behind them.
