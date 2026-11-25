# Picture cost study for 0.9.5 (in progress)

Started 2026-11-25 after 0.9.4, applying what the GA36 port taught about where a
frame's time goes. The rule for every change: the pictures stay the same. Each
change is checked with the picture hash of `SS_TEST_RENDER` (every frame drawn,
reference script `tools/tests/play_input.txt`) and the 160-row game hash
(`tools/ga36_hash_reference.py --check`).

## Tool

`tools/perf_bench.py --a DIR_A --b DIR_B [--frames N] [--repeat R] [--views ...]`
runs two Windows 64-bit builds headless on the reference script, alternating A
and B, at each view (240x160, 320x160, 320x240 Flat, 480x160), with `SS_PROF`.
It prints the fastest run of each side (the machine's other work only adds
time) per stage, and whether both drew the same pictures. Keep baseline builds
in `build/perf/<name>/` (ss2_win64.exe, ss2_game.elf, SDL3.dll).

## Where a frame's time goes (0.9.4, Windows x64)

`SS_PROF` per frame, 320 columns, 1,200 frames: picture (`ss_ppu_render`)
1,145 µs mean, `finish_picture` 286 µs, sound 35 µs, game logic 8 µs. The
picture is almost all of it.

Inside `ss_ppu_render` (temporary `rdtsc` timers, cycles per frame, 6,000
frames):

| View | sprite tracking | backgrounds | sprites | per-pixel compose |
|---|---|---|---|---|
| 240x160 | 16 k | 742 k | 131 k | 1,857 k |
| 320x240 | 15 k | 898 k | 155 k | 2,889 k |

Composition is about two thirds: 40-50 cycles a pixel.

## Changes

1. **Sprite rows once a frame.** Each row looked at all 128 sprite table
   entries and decoded each one to find out it did not reach the row. Now
   `obj_place()` computes an entry's size, box and first row (with the raised
   status display and the lowered message window), `sprite_rows()` keeps every
   entry's row range once a frame, and a row only calls `sprite()` for the
   entries that reach it. `sprite()` uses the same `obj_place()`, so both agree.
   Pictures identical on every view.
2. **Plain pixels.** `compose_plain()` draws the common pixel (a row without the
   display's windows, no edge fade, inside the map, front-most layer with no
   semi-transparent sprite or alpha blend) in fewer steps; every other pixel
   still goes through `compose()`. Measuring.

## Results

`perf_bench.py`, 0.9.4 (A) against changes 1 and 2 (B), fastest of 3 runs of
8,000 frames each, picture stage in µs per frame. Same pictures on every view.

| View | A | B | Change |
|---|---|---|---|
| 320x160 | 1,070 | 1,033 | -3.4% |
| 320x240 (Flat) | 1,566 | 1,517 | -3.1% |
| 480x160 | 1,288 | 1,277 | -0.9% |
| 240x160 | 804 | 1,303 | discarded: `finish_picture` rose by the same 63%, so the machine was loaded during B's runs |

So far about 3%: the branches these two changes save are not where the
composition's time is. The machine is shared with other work; repeat the 240
case before reading it.

## Next candidates

- Window masks once a row instead of six register reads a pixel.
- Backgrounds: tile runs from the source maps (a quarter of the time).
- Android, Linux and GA36 share this core: they gain the same.
