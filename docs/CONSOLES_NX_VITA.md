# Nintendo Switch and PS Vita (homebrew)

State on 2026-11-13, base 1ea9afe (v0.8.4). **Nothing here has run on a console.** The Vita package builds and
is well-formed by the SDK's tools; the Switch has a design and a proved memory scheme but no program yet. What
was checked and what was not is listed at the end.

## 1. How the game gets its memory

Every other build gives the game its memory at the GBA's real addresses (`0x02000000` EWRAM ... `0x08000000`
ROM): the decompiled C names 750 variables by address and the ROM's data is full of pointers. Neither console
allows that:

| | Why the real addresses are out | What ReKHCoM does there |
|---|---|---|
| Switch | A homebrew program is 64-bit; the lowest address its code region can have is `0x08000000`, and it is placed at random. `svcMapProcessCodeMemory` (title takeover mode) can map an image at a chosen address at or above that, never below. | The game is an `arm64_32` image (AArch64 code, 32-bit pointers, Mach-O) linked at `0x10000000` and mapped there; the GBA regions are arrays of the image (`PORT_GBA_RELOC`) and the ROM's pointer words, listed by a generator, are rewritten when the ROM is loaded. |
| Vita | 32-bit ARM, so pointers fit natively, but user memory starts at `0x81000000` and a user program cannot choose the address of a memory block. | The same relocatable regions; the game is ordinary ARMv7 code linked into the program, calling the host through the same bridge as plain function pointers. |

ReKHCoM's relocation needs a complete list of the pointer words in the ROM. ShiningSource has no such list
(only the ranges that hold pointers to *code*), and making one for a second game is the slow part. The web
build shows the other way: `tools/absolute_ir.py` turns the fixed addresses into constants in the compiler's
intermediate code. WebAssembly can then use them as they are, because its memory starts at 0; a console needs
one more step, a base added to them. That step is `tools/rebase_ir.py`, and it is the design for both consoles:

- The regions lie one after another in one block of the program, `ss_arena` (`runtime/console/rebase.c`):
  512 KiB for each of the regions 02..07, then 16 MiB for the ROM; 19 MiB in all.
- **The game's modules.** Every pointer the game holds, in memory or in a register, keeps the cartridge's
  value, so the ROM's data is used as it is and none of the decompilation's habits matter (a pointer made
  from an offset and moved by an address, a parameter that is a number in one file and a pointer in another).
  The address is moved into the block where memory is touched: a load or store through anything that is not
  plainly a local or a global looks up the top byte of its address in a 256-entry table and adds what it
  finds (0 for addresses of no GBA region: the program's own data and stack). Addresses that are constants,
  12,000 of them, become `ss_arena + offset` at link time and cost nothing.
- **The runtime core** (renderer, sound, BIOS calls) is written for the port and keeps its types straight, so
  its inner loops do not pay per access: there a pointer in a register is the real place, and it is converted
  when it is read from memory or made from a number. Where the core and the game call each other, pointers
  travel as the cartridge's values.
- Nothing is mapped, so nothing depends on what the console's kernel allows.

**Stray accesses.** The cartridge reads and writes through null and through one bad pointer (about 215,000
times in the reference run); the GBA ignores it and the other builds absorb the fault in a handler. The
consoles do not need one, and do not rely on the homebrew environment delivering faults: the same test point
that moves an address also asks whether it lies below the program's memory (below `0x10000000` on the
consoles). Such a read gives 0 and such a write goes to a scratch block, exactly what the fault handlers do; a
block copy reads zeros or writes to the scratch block. Divisions get the cartridge's results where they are
compiled (`tools/division_ir.py`, as in the web build), so no processor's behaviour on a zero divisor is used.

**Cost.** In the game's code, each load or store through a pointer is a shift, a table read, an addition and
two selects more (28,700 places). On the PC check build the whole reference run takes 5.3 s against 13 s for
the development build, which pays for 215,000 faults instead; the game's logic is a small part of a frame, so
the cost on a console should be small next to the renderer, which is untouched. It has not been measured on a
console. Memory: 19 MiB for the block.

**Risks.** (1) The program's own memory must not have a top byte of 02..08, or its addresses would be taken
for the GBA's. True on the Vita (`0x81000000` and up) and for a Switch image linked at `0x10000000`;
`ss_memory_init` checks the block itself. (2) An access more than 512 KiB into a region would land in the next
one; the hardware regions are at most 256 KiB and the reference run agrees with the other builds, which is
evidence, not proof, for the rest of the game. (3) On the consoles any address below `0x10000000` that is not
the GBA's is treated as a stray access; a real bug of the port in that range would be silent there (the PC
check build is strict and reports it).

### The proof on this PC

`tools/build_rebased.py` builds the development program with this scheme (32-bit Windows, same sources, same
platform layer): `build/rebased/ss2/ss2_rebased.exe`. In it the GBA's own address range is closed (every page
inaccessible, `runtime/console/check_win32.c`) and no fault is absorbed, so an access that still used a
hardware address, or a stray access the test missed, stops the program. It prints the reference:

    SS_SKIP=0 SS_FRAMES=32000 SS_INPUT_FILE=input_play.txt build/rebased/ss2/ss2_rebased.exe rom.gba
    frame 32000  hash 5F1DB1A8  stubs reached 0
    audio: 25716797 sample frames, peak 10279, 35889340 samples above the noise floor

That is x86 code. The console builds run the same rewritten intermediate code through the ARM back ends; the
reference cannot be run on a console here, and the ARM code itself has not been run (see "not verified").

## 2. PS Vita

Design: the "guest" of the 64-bit builds without the 64-bit part. `tools/build_vita.py guest` compiles the
game, the runtime core and the image's own small C library (`runtime/guest`) with clang for the Cortex-A9
(ARMv7, Thumb-2, hard float) into one object whose only visible names are `ss_guest_entry` and `ss_guest_rom`.
`tools/build_vita.py package` compiles `runtime/console/vita_host.c` in ReKHCoM's consoles build machine
(VitaSDK and its SDL2; `tools/console_vm.py` opens the machine's image with `snapshot=on`, so it is never
changed, and gives it no network), links both and packs `build/vita/ShiningSource.vpk`. The game reaches the
console through the bridge table of `runtime/guest/bridge.h`, here a table of plain functions.

| | |
|---|---|
| ROM | the user's own dump: any `.gba` file in `ux0:data/ShiningSource/`. Nothing of the game is in the package. |
| Saves | `ux0:data/ShiningSource/shiningsource.sav`, the cartridge's format |
| Settings | `ux0:data/ShiningSource/settings.txt`, optional, lines `NAME=VALUE` with the port's `SS_` variables |
| Log | `ux0:data/ShiningSource/log.txt` |
| Picture | 960 x 544: the wide view at 320 columns x 3 = 960 x 480, the whole width of the screen with 32 lines of border above and below. Scenes that keep the console's composition are 240 columns x 3, centred. SDL2 renderer, nearest pixels. |
| Sound | 48 kHz stereo through SDL2; the game is paced by what the sound device has taken |
| Buttons | Circle = A, Cross = B (the GBA's positions), L, R, Start, Select, D-pad or left stick |
| Package | title id `SSRC00001`, version 00.84, the port's own icon and LiveArea pictures drawn by `tools/make_icon.py` |

Not there yet: the options menu, the launcher, the larger status display, notices, online play (the bridge
answers "no network"), achievements. `plat_sdl.c` is not used: it is written for SDL3 and the SDKs on this PC
carry SDL2 only (VitaSDK, devkitPro); the host draws with SDL2 directly, as ReKHCoM's console host does.

To try it on a Vita (HENkaku/Ensō, VitaShell):

1. Copy `build/vita/ShiningSource.vpk` to the Vita and install it with VitaShell.
2. Create `ux0:data/ShiningSource/` and copy your own Shining Soul II (Europe) dump there, any name ending in `.gba`.
3. Start ShiningSource. A dark red screen for five seconds means no ROM was found.
4. Whatever happens, `ux0:data/ShiningSource/log.txt` says how far it got: send that file back.

## 3. Nintendo Switch

Design: the existing ARM64 game image (`tools/build_arm64.py`: `arm64_32`, Mach-O, already running on Android
and ARM64 Linux) with the rebased memory, linked at `0x10000000`, and a libnx host that maps it there with
`svcMapProcessCodeMemory` as ReKHCoM's `port/switch/host/switch_host.c` does (title takeover mode), serves the
bridge and draws at 1280 x 720 (320 columns x 4) or 1920 x 1080 docked (x 6, 60 lines of border). The stray
test and the divisions are as on the Vita. devkitA64, libnx and SDL2 are in the build machine
(`/opt/devkitpro`); nothing is missing on this PC.

**Not built.** The time went into the memory scheme and the Vita. What is left, in order:
`tools/build_switch.py` (the image through `tools/rebased.py` with the Mach-O names; `rebase_ir` already
handles the name prefix), the way the ROM path reaches the image (`ss_guest_rom` is a plain symbol on the Vita;
an image needs it in the entry call), `runtime/console/switch_host.c` (map the image, the bridge functions of
`vita_host.c` with libnx's pad instead of `sceCtrl`, the stack the image runs on), the `.nro` with the icon
through `elf2nro`/`nacptool`. An ARM64 Linux test host of the same image in the ARM64 test machine would run
the reference before a console does, as ReKHCoM's Switch build does.

## 4. What was checked, and what was not

Checked on this PC:

- The memory scheme, as x86 code: the reference lines above, with the GBA's addresses closed and no fault absorbed.
- The existing builds on the final tree: `build/dev32/ss2/ss2_dev32.exe` and `build/win64/ss2/ss2_win64.exe`,
  rebuilt, print the same two lines. The only shared file changed is `runtime/guest/guest.c` (guards).
- Vita: 43 game modules, the core and the guest library compile for ARMv7 and link into one object that needs
  only compiler helpers from outside (`build/vita/ss2/guest-needs.txt`); the program links against VitaSDK with
  no undefined name but five weak ones of the C start-up; `vita-elf-create` accepts it (every imported
  function is known to the SDK's database), `vita-make-fself`, `vita-mksfoex` and `vita-pack-vpk` produce the
  package; its list and the program's segments (code at `0x81000000`, 24 MiB of data) are in
  `build/vita/ss2/vm-out/check.txt`.

Not verified:

- **It has never run on a Vita, an emulator or any ARM processor.** The rewritten code was run as x86 only.
  Specific to ARM and unproved: the coroutine switch (`runtime/console/co_arm.S`), the ARM back end's code for
  the game, the compiler helpers for division on a processor without a divide instruction.
- Everything in `vita_host.c`: SDL2 on the Vita, the picture, the sound and its pacing, the buttons, the file
  paths, the heap and stack sizes, whether 24 MiB of data plus SDL2 fit the default memory budget.
- Speed on either console.
- The Switch: nothing beyond the scheme it shares.
