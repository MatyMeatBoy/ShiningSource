# OptiPort passes: work board

Two passes with the OptiPort method (github.com/MatyMeatBoy/OptiPort, guidance read at
commit 35dfad8 both times). This board is the record: what was measured, what
changed, what is still open. Numbers are from the named workload on the named
machine and say nothing about other devices.

## Workloads and what must not change

- Workload A (correctness): the scripted play-through, 32,000 frames headless
  (`SS_SKIP=0 SS_FRAMES=32000 SS_INPUT_FILE=build/stats/input_play.txt`). The
  final frame hash is `5F1DB1A8` and the audio line is `25716797 sample frames,
  peak 10279, 35889340 samples above the noise floor` (first pass: `5711719E`,
  before the sound engine and the script changed). Every change is re-checked
  against both.
- Workload A-picture (second pass): the same run with `SS_TEST_RENDER=W`, which
  draws and finishes every frame W columns wide as a window would and prints one
  hash over all 32,000 pictures: `6C769772` at 240, `CCC74CA2` at 427,
  `9778CAF9` at 480. `SS_PROF=22000:32000` times the stages over the busy field
  scene at the end of the script (three enemies, wide map).
- Workload B (pacing and memory): the same script with a window, sound device
  open at volume 0, `SS_STATS=1 SS_QUIT=N`: 5,400 frames (90 s) as in the first
  pass, 7,200 for sound, 27,000 (7.5 minutes, into the field) for the long run.
- Contract: the game's own logic and timing (59.7275 frames a second), the save
  format, identical results on every processor (online depends on it).

## First pass (2026-11-12): done and verified

| Area | Before | After | Evidence |
| --- | --- | --- | --- |
| Memory charged to the process, Windows x64 | 281.0 MiB committed, 69.2 MiB in use | 95.1 MiB committed, 69.0 MiB in use | Workload B, `stats:` lines. The low address block is now a read-only section; each range becomes writable when handed out. Hash unchanged. |
| Program size, Windows x64 host | 178 KiB | 178 KiB | The section has no bytes in the file (a first attempt made it 235 MB; corrected before commit). |
| Frame pacing, Windows x64 | mean 16.74 ms, p95 16.74, p99 16.98, max 18.07, 0 late of 5,398 | same within noise (p99 16.99, max 18.17) | Workload B. Nothing to fix here: 90% of the time is spent waiting for the next frame. Measured so that later changes can be compared. |
| Crash report | console only (lost when started from the launcher) | console and `shiningsource_crash.log` next to the program, with registers, the game addresses on the stack, time and host build; bounded at 256 KiB plus one old file | `SS_TEST_FAULT=crash`: report written, exit code 3. `tools/crash_names.py` names the functions. |
| Hangs | none detected | a watchdog reports where the game's thread is after 10 s without a frame or a network wait, once per hang, and lets it continue | `SS_TEST_FAULT=hang`: report written after 10 s. Waiting for an online player counts as alive, so it is not reported. |
| Save file | written to a temporary file, old save deleted, then renamed; a failed write was not noticed | the write is checked; the replaced save is kept as `.bak`; a missing or short save loads the backup; a write that fails leaves the save alone | `tools/tests/run_save_test.sh`: 8 checks pass (complete write, replacement, cut-short file, missing file, unwritable folder). |
| Identical results on ARM64 | first difference at frame 2,702 | 0 of 32,000 frames differ | Debian 12 ARM64 under QEMU, `SS_HASH_ALL=1` compared line by line with x86-64. Three causes, all in how arguments cross calls (docs/ROADMAP.md, M5). |
| Division by zero | quotient 0 and remainder 0 on x86, the dividend as remainder on ARM64 | the cartridge's rule everywhere (0 and 0), by fault handler on x86 and by rewriting the IR on ARM64 | Read from the ROM's division routines; Workload A hash unchanged on x86 and ARM64. A first attempt unified on ARM64's own result, which is not the cartridge's; corrected. |
| Long test scripts | passed in an environment variable, silently dropped above about 32 KiB by the test script | `SS_INPUT_FILE` | A 40 KiB script now reaches four players; before, two runs were judged on stale logs. |

## Second pass (2026-11-13, source at ca9a74a)

Over what 0.4 to 0.7 added: the picture's finishing step, the options menu, the
game's typeface, the new sound engine, the message channel, achievements, Rebel
mode. Machine: Ryzen 7 7435HS laptop, RTX 4060, 144 Hz panel, Windows 11; other
programs were running, so single runs differed by up to a factor of two. Stage
times therefore come from runs at high priority, before and after measured at
the same time, medians of three rounds; windowed figures from the two builds
running side by side.

### Tools added (inert unless asked for)

- `SS_PROF=1` or `SS_PROF=F:T`: mean, median, 99th percentile and maximum per
  stage of a frame (game logic, sound, link, blank handlers, cheats and
  achievements, picture, find_flicker, finish_picture, presenting), printed at
  the end. Works on x86 and ARM64 (the processor's counter), so it can be run
  on a phone. `ss_prof(stage, begin)` in `runtime.h`.
- `SS_TEST_RENDER=W`: see Workload A-picture.
- `SS_STATS=1` now also prints: time to the first picture and its parts, the
  shortest interval and how many were early, the cost of texture upload,
  drawing calls and the present call, and the sound queue (mean, least, most,
  how often empty, frames dropped, frames with trimmed speed).
- `SS_TEST_FAULT=thread`: a crash on another thread than the game's.
- `SS_TEST_KEYS`: `min`, `restore` (minimise the window, bring it back) and
  `reset` (make the textures again, as after a graphics device reset).
- `tools/crash_names.py` names host functions too.

### Frame cost per stage, busy field scene (microseconds, mean / p99)

Headless with `SS_TEST_RENDER`, final build alone at high priority:

| Stage | 240 columns | 427 columns | 480 columns |
| --- | --- | --- | --- |
| game logic | 49 / 238 | 40 / 93 | 36 / 73 |
| sound (m4aSoundMain) | 51 / 103 | 49 / 98 | 49 / 98 |
| picture (ss_ppu_render) | 1,763 / 2,964 | 2,356 / 4,490 | 2,453 / 4,829 |
| of it find_flicker | 1.5 / 4 | 1.5 / 4 | 1.8 / 5 |
| finish_picture | 71 / 540 (one run; 73 / 114 below) | 95 / 175 | 101 / 175 |
| share of the 16.74 ms frame, all of the above | 11.6% | 15.2% | 15.8% |

Before and after, both builds at the same time (slower than alone: two runs
share the machine):

| finish_picture | before | after |
| --- | --- | --- |
| 240 columns | 231 / 334 | 73 / 114 |
| 427 columns | 391 / 625 | 146 / 228 |
| 480 columns | 436 / 699 | 176 / 270 |

With a window, 480 columns, 27,000 frames, stages over frames 22,000 to 27,000,
both builds side by side:

| Stage | before | after |
| --- | --- | --- |
| picture (ss_ppu_render), same code in both | 1,974 / 2,965 | 1,873 / 2,796 |
| finish_picture | 225 / 371 | 93 / 156 |
| texture upload | 98 / 212 | 94 / 205 |
| drawing calls (the side field, overlay, menu) | 164 / 245 | 4 / 8 |
| present call | 313 / 549 | 256 / 512 |
| work per frame, sum of means with logic and sound | 2.87 ms (17.2%) | 2.42 ms (14.5%) |

Upload, drawing and present call by width (before, VSync off): 125, 4, 319 at
240; 196, 203, 529 at 427; 165, 197, 404 at 480.

What a core 5 to 10 times slower would spend (this machine's means times 5 and
10; an estimate, not a measurement): at 480 columns 14 to 29 ms before and 12
to 24 ms after; the renderer alone 9 to 20 ms at 427 and 480, 9 to 18 ms at
240. Everything else together is 0.55 ms here after this pass (2.7 to 5.5 ms
there). So on such a core the frame does not fit 16.7 ms because of
`ss_ppu_render`, in the wide view at any factor above 5.5 and in the classic
view above 7, and its 99th percentile (4.5 to 4.8 ms here) does not fit at 5.
Inside the renderer, measured per line at 240 columns: the backgrounds 37%, the
sprites 8%, the composition of the pixel 55%.

### Done and verified

| Area | Before | After | Evidence |
| --- | --- | --- | --- |
| finish_picture (core/frame.c) | four full-picture history buffers shifted per pixel every frame, a branch per pattern | the buffers form a ring (one copy a frame), the brightness sample is its own loop, the comparison has no branch and the compiler vectorises it | Tables above. All 32,000 pictures identical at 240, 427 and 480 columns (the three hashes), and six window pictures identical byte for byte. |
| The side field (plat_sdl.c draw_field) | about 1,000 rectangles a frame in the enhanced mode, also in the field where the picture covers them | computed once per shape, kept as a texture; the rectangles remain as the way out if the texture cannot be made | Drawing calls 164 to 4 microseconds a frame at 480; window pictures identical in enhanced and classic mode (border "Soul"), menu open and closed. |
| Memory charged, Windows x64 | Workload B: 96.6 MiB committed, 68.7 in use (first pass: 95.1 and 69.0). Headless after 32,000 frames: 42.0 MiB private, 26.7 in use | 92.2 and 69.7; headless 38.0 and 26.8 | The call trace's 4 MiB buffer (core/trace.c) was in every build; now only in a trace build (`SHININGSOURCE_TRACE`, tools/build.py). The image's range went from 8.0 to 4.06 MiB. One picture buffer serves the window and the render test. |
| Memory over time | not measured | flat | 7.5 minutes at 480 columns: committed 100.1 MiB after one minute, 100.4 after seven; in use 72.8 and 71.8; handles 666 to 676; threads 63 to 67. |
| Game speed when the display does not pace as announced (plat_sdl.c ss_plat_present) | with "paced by the display" the game ran as fast as pictures could be shown: 6.94 ms a frame (2.4 times too fast) with a 144 Hz panel taken for 60 Hz, 13.88 ms taken for 120 Hz; 1,044 and 295 of 1,800 frames of sound dropped | a present call that returns more than 2.5 ms before a frame's time is followed by the game's own clock; after 30 such frames the display is no longer the pace and the sound returns to its own speed; decided again when the window is restored or the display changes | `SS_TEST_REFRESH=60` and `120` on the 144 Hz panel: 16.74 ms, p99 16.83, 0 early, 0 dropped. The same path covers a minimised window and a driver told to ignore synchronisation on a real 60 Hz display; those two were not reproduced here. |
| Sound reserve (plat_sdl.c ss_plat_audio) | the queue held 2 to 7 ms when a frame's samples arrived, least 0.0: any frame more than a few milliseconds late left the device without sound. Empty 1 time in 2 minutes, 4 times in 30 s in two other runs, 10 times in the 7.5 minute run with 1 frame dropped | 36 ms of silence go in first whenever the queue was dry after a pause (start, options menu, skipped scene); the average is held between 20 and 55 ms by playing 0.4% slower or faster when it leaves that range | 30 to 42 ms on average, least 26 to 34 ms, empty 0 times, dropped 0, speed trimmed in 0 frames in every run without a pause (2 minutes, 7.5 minutes, VSync off, pretend 50 and 60 Hz). Costs 36 ms of latency. Not listened to: every run was at volume 0. |
| Rebel save (core/rebel.c use_rebel_save) | a Rebel save cut short with a whole `.bak` beside it was replaced by a copy of the normal save: the Rebel run's progress was gone at the next write | the previous Rebel save counts as the run's save; flash.c loads it | Test with marked files: before, `shiningsource_rebel.sav` became the normal save's bytes; after, the previous Rebel save is loaded ("the previous save was loaded"). `run_save_test.sh` still 8 of 8. |
| Network log (core/netlink.c net_say) | `shiningsource_net.log` appended for ever | past 256 KiB it becomes `shiningsource_net.old.log` and a new one starts; if the rename fails the old file is appended to and the change tried again later | A 345 KiB log and one session: old file 345,000 bytes, new file one line. With the old name blocked: the line is appended, nothing lost. |
| Crash reports | did not say which thread; host addresses had no names | "the game's thread" or "another thread"; crash_names.py names host functions | `SS_TEST_FAULT=crash`, `thread`, `hang`: all three written; the thread crash is reported as `faulting_thread+0x0 (host), another thread`, exit code 3. |
| Graphics device reset | the picture's texture, the typeface and the notice icon were made once | made again on `SDL_EVENT_RENDER_DEVICE_RESET`; the frame at hand is uploaded again, also behind the menu | `SS_TEST_KEYS=...:reset` three times in a run, once with the menu open: window pictures identical to a run without. The event itself did not occur here. |

Still holding with today's features: the hang watchdog stays quiet while the
options menu pauses the game for 20 s and during a skipped scene (165 frames
run ahead, a picture every 30); closing the menu causes no burst and no jump
(shortest interval 15.7 ms, longest 17.8, 1,799 pictures for 599 game frames);
a minimised window keeps 16.74 ms on the game's own clock.

### Measured and left alone

- Frame pacing on this display (144 Hz, VSync on: the game's clock, each
  picture waits for the refresh): 7,198 frames, mean 16.74 ms, p50 16.74, p95
  16.74, p99 16.91, max 18.01, 0 late. VSync off: p99 16.85, max 17.40. Variable
  refresh is refused by this display and falls back to no synchronisation.
  A pretend 50 Hz display: no VSync, 16.74 ms.
- `ss_ppu_render`: the cost above. Another person is rewriting its sprite code;
  nothing was changed. A first attempt (finding the order of the backgrounds
  once a line instead of once a pixel) measured no gain (3.12 against 3.44 ms
  with the two builds side by side) and was dropped.
- Sound: `m4aSoundMain` 46 to 51 microseconds a frame, p99 about 100 (0.3%).
- Link: with no session nothing is sent and no socket exists (it is opened when
  the game's link task starts); 0.1 to 0.3 microseconds a frame, p99 1. With a
  session open and no other player: 12 microseconds, p99 93. Entering
  multiplayer costs one frame of 29 ms: the room's name includes a hash of the
  16 MiB ROM. Changing the hash would change the rooms, so it stays.
- Achievements: 0.2 to 0.8 microseconds a frame with the port's own list (p99
  under 3); the frame of an unlock 0.66 ms. Not measured with a
  RetroAchievements set (none was cached in the test folder) nor signed in.
- Start: 95 ms from process start to the first frame without a window. With a
  window 620 to 650 ms to the first picture, warm: 40 to 78 ms before the
  window code (ROM, image), SDL 62 to 81, the sound device 165 to 300, window
  and renderer 255 to 345, the first frame 6 to 8. The first run of a newly
  built program took 760 ms, and its working set was 277 MiB for that run only
  (something read the whole read-only block once, probably the virus scanner;
  committed memory was unchanged). A start after a reboot was not measured.
- Saving writes the whole 64 KiB file once per sector the game programs. No
  late frame was seen in a save; not changed.

### What the files hold

- `shiningsource_net.log` (working folder, bounded as above): the system's
  millisecond counter, frame, seat, event; a session's first line names the
  relay's address and the room, which the user typed.
- `shiningsource_crash.log` (next to the program, 256 KiB and one old file):
  addresses, registers, UTC time, the host's build time. No paths, no names.
- `rom_path.txt` next to the program: the ROM's path as the user chose it.
- The launcher's folder (`SS_RA_DIR`): the sign-in token, encrypted for this
  Windows user; `retroachievements_pending.txt` (the account's name, at most
  128 lines); `local_record.txt` and `rebel_record.txt` (one line per
  achievement, each only once); the set and its badge images (one per ROM).
  None grows without limit.

## Open

- The renderer on a slow phone, see the estimate above. Two ways, neither
  taken here: make `ss_ppu_render` cheaper (fetch a background by tile, a short
  path for pixels with no window and no blend), which belongs to the rewrite
  under way; and show every second picture when the device cannot hold the
  game's speed. The second needs the platform to count a frame that was not
  shown (ss_plat_present advances its schedule per picture today) and a phone
  to tune it on.
- Nothing here ran on a phone: stage times, pacing, the sound reserve against a
  larger device buffer, and the texture path after the app comes back are
  unverified there. `SS_PROF=1` and `SS_STATS=1` give the figures on a device.
- A real 60 Hz display: "paced by the display" was only reached by pretending.
- The window held by its title bar for more than 10 s would be reported once
  as a hang (Windows runs its own loop on the game's thread). It could not be
  reproduced from a script without moving the user's pointer; not changed.
- The sound has to be listened to once with the reserve.
- Memory in use (69 MiB): 16 MiB is the ROM copy, 4 MiB the main stack made
  writable at once, 0.5 MiB per coroutine stack. Not measured as a problem.
- dev32 (the 32-bit development build) still commits its whole 128 MiB block.
- Crash and hang reports exist only in the Windows host. Linux prints a crash to
  the terminal; Android writes its log to the app's folder but has no watchdog.
- Online: intervals under real latency are measured only through the relay
  simulation (docs/ROADMAP.md, M7).

## After the second pass: the renderer (2026-11-13)

The second pass left `ss_ppu_render` alone and named it as what keeps a slow
device from fitting a frame. Timers around its parts (removed again) gave, per
frame over the whole 32,000-frame reference run: backgrounds 634 us, sprites
135, composing the pixels 625 at 240 columns; 818, 136 and 1,164 at 480.

- **Backgrounds a tile row at a time** (`tile_run`, `hardware_span`,
  `source_span` in `runtime/core/ppu.c`): the map entry and the tile's row are
  found once for up to eight pixels, and an empty row is written in one go.
  634 -> 279 us at 240 columns, 818 -> 381 at 480.
- **Composition from the front**: the line's layer order is worked out once per
  line, and a pixel stops at the first layer that shows something; the layer
  under it is only looked for when a blend needs it. 625 -> 473 us, 1,164 -> 931.

| `ss_ppu_render`, frames 22,000 to 32,000, mean / p99 (us) | before | after |
|---|---|---|
| 240 columns | 1,398 / 2,667 | 889 / 1,738 |
| 480 columns | 2,079 / 4,322 | 1,381 / 2,414 |

Same machine and method as above (noisy: single runs, Windows x64). Proof that
the picture is the same: `SS_TEST_RENDER` hashes over all 32,000 pictures are
identical before and after at 240, 284, 427 and 480 columns (6CEBF2FF,
046826C4, 168EA6A0, 10328074), the reference hash is unchanged, and the
transparency bench still has no failing case. What the script does not show
(256-colour tiles, 512-pixel hardware maps) is covered only by the code being
a transcription of the per-pixel version.

Tried and dropped, no measurable gain: composing each line into local buffers.

## A machine too slow for every picture (2026-11-13)

Before: when a picture took longer than a frame, the game itself slowed down
and the sound device ran dry, because every frame waited for its picture.

Now the platform keeps the console's clock for every game frame, whoever paces
the game, and says so when a frame ends more than one frame behind
(`SS_PRESENT_LATE`). The next frame then goes by without its picture
(`ss_plat_present` with width 0: events, buttons and the clock only), at most
three in a row. The renderer still looks at such a frame's sprites
(`ss_ppu_unseen`), because which sprites alternate is told from every frame in
a row. `SS_FRAMESKIP=0` turns it off.

Test: `SS_TEST_SLOW=ms` makes every picture take that much longer. 1,500
frames with a window, Windows x64:

| picture slowed by | game frames a second, before | after | pictures left out | sound queue empty |
|---|---|---|---|---|
| 0 or 12 ms | 59.69 | 59.69 | 0 | 0 times |
| 25 ms | 37.91 (sound reserve 0.1 ms throughout) | 59.63 | 546 of 1,500 | 0 times |
| 40 ms | not measured | 59.62 | 893 of 1,500 | once |

The same with `SS_TEST_REFRESH=60`: 59.62 a second, 748 left out. Not verified:
a display that really paces the game at 60 Hz on a slow device (this panel is
144 Hz and the simulated case falls back to the port's own clock), and a real
phone. Per-pixel smoothing of blinking that is not a sprite sees only the
pictures that are drawn, so it can miss while pictures are being left out.

