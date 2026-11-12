# OptiPort pass: work board

A pass with the OptiPort method (github.com/MatyMeatBoy/OptiPort, guidance read at
commit 35dfad8) over memory, crash and hang reports, frame pacing, save
protection and the 32-to-64-bit work. This board is the record: what was
measured, what changed, what is still open. Numbers are from the named workload
on the named machine and say nothing about other devices.

## Workload and what must not change

- Workload A (correctness): the scripted play-through, 32,000 frames headless
  (`SS_FRAMES=32000`, `build/input_play.txt`). The final frame hash is
  `5711719E` on every build; every change below was re-checked against it.
- Workload B (pacing and memory): the first 5,400 frames (90 s) of the same
  script with a window, sound device open at volume 0
  (`SS_STATS=1 SS_QUIT=5400`), Windows 11 x64, desktop PC.
- Contract: the game's own logic and timing (59.7275 frames a second), the save
  format, identical results on every processor (online depends on it).

## Done and verified

| Area | Before | After | Evidence |
| --- | --- | --- | --- |
| Memory charged to the process, Windows x64 | 281.0 MiB committed, 69.2 MiB in use | 95.1 MiB committed, 69.0 MiB in use | Workload B, `stats:` lines. The low address block is now a read-only section; each range becomes writable when handed out. Hash unchanged. |
| Program size, Windows x64 host | 178 KiB | 178 KiB | The section has no bytes in the file (a first attempt made it 235 MB; corrected before commit). |
| Frame pacing, Windows x64 | mean 16.74 ms, p95 16.74, p99 16.98, max 18.07, 0 late of 5,398 | same within noise (p99 16.99, max 18.17) | Workload B. Nothing to fix here: 90% of the time is spent waiting for the next frame. Measured so that later changes can be compared. |
| Crash report | console only (lost when started from the launcher) | console and `shiningsource_crash.log` next to the program, with registers, the game addresses on the stack, time and host build; bounded at 256 KiB plus one old file | `SS_TEST_FAULT=crash`: report written, exit code 3. `tools/crash_names.py` names the functions. |
| Hangs | none detected | a watchdog reports where the game's thread is after 10 s without a frame or a network wait, once per hang, and lets it continue | `SS_TEST_FAULT=hang`: report written after 10 s. Waiting for an online player counts as alive, so it is not reported. |
| Save file | written to a temporary file, old save deleted, then renamed; a failed write was not noticed | the write is checked; the replaced save is kept as `.bak`; a missing or short save loads the backup; a write that fails leaves the save alone | `tools/tests/run_save_test.sh`: 8 checks pass (complete write, replacement, cut-short file, missing file, unwritable folder). |
| Identical results on ARM64 | first difference at frame 2,702 | 0 of 32,000 frames differ | Debian 12 ARM64 under QEMU, `SS_HASH_ALL=1` compared line by line with x86-64. Three causes, all in how arguments cross calls (docs/ROADMAP.md, M5). |
| Division that traps | quotient 0 and remainder 0 on x86, processor's own result on ARM64 | one rule everywhere (ARM64's) | Workload A hash unchanged on x86; ARM64 identical. |
| Long test scripts | passed in an environment variable, silently dropped above about 32 KiB by the test script | `SS_INPUT_FILE` | A 40 KiB script now reaches four players; before, two runs were judged on stale logs. |

## Open

- Memory in use (69 MiB): 16 MiB is the ROM copy, 4 MiB the main stack made
  writable at once, 0.5 MiB per coroutine stack. Mapping the ROM file instead of
  copying it is not possible inside the program's own image on Windows; the
  stacks could be made writable page by page. Not measured as a problem.
- dev32 (the 32-bit development build) still commits its whole 128 MiB block.
  It is a developer tool; left as it is.
- Linux and Android hosts: memory is mapped range by range already. Not measured
  with `SS_STATS` on a real device; pacing on a phone is unknown.
- Crash and hang reports exist only in the Windows host. Linux prints a crash to
  the terminal; Android writes its log to the app's folder but has no watchdog.
- Android lifecycle (pause, resume, losing the surface) relies on SDL's
  defaults and has not been exercised.
- Sound: the queue is capped at a tenth of a second; underruns are not counted.
- Online: delay is fixed per session; intervals under real latency are measured
  only through the relay simulation (docs/ROADMAP.md, M7).
