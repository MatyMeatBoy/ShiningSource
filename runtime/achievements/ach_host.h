/* Achievements in the game process: see ach_host.c. Built into the Windows host; a host without them serves an
 * empty function for the frame's call (runtime/host64/host_linux.c). */
#ifndef SS_ACH_HOST_H
#define SS_ACH_HOST_H

#include <stddef.h>
#include <stdint.h>

/* Once, before the game starts, with the ROM as it was loaded. */
void ss_ach_start(const uint8_t *rom, size_t size);
/* Once per game frame, on the game's thread; `run_flags` are the SS_RUN_* of runtime.h. Never waits. */
void ss_ach_frame(int run_flags);

#endif
