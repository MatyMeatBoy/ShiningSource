/* Achievements in the game process: see ach_host.c. Built into the Windows host and the Linux hosts (GA36, R36S,
 * Linux x64); a host without them serves an empty function for the frame's call. */
#ifndef SS_ACH_HOST_H
#define SS_ACH_HOST_H

#include <stddef.h>
#include <stdint.h>

/* Once, before the first frame, with the ROM as it was loaded (before the game patches it). The bytes are only
 * hashed: the caller may free them afterwards. */
void ss_ach_start(const uint8_t *rom, size_t size);
/* The same with the ROM file, read and let go again (hosts whose image loads the ROM itself). 0: not readable. */
int ss_ach_start_file(const char *path);
/* Where the game's external and internal work RAM are, for a host whose image keeps them away from the console's
 * addresses (a rebased image). Before ss_ach_start. Without it the console's own addresses are read. */
void ss_ach_set_memory(const uint8_t *ewram, const uint8_t *iwram);
/* Once per game frame, on the game's thread; `run_flags` are the SS_RUN_* of runtime.h. Never waits. */
void ss_ach_frame(int run_flags);

#endif
