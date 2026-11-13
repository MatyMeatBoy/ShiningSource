/* GBA memory away from its hardware addresses (Switch, Vita): see rebase.c and tools/rebase_ir.py. */
#ifndef SS_REBASE_H
#define SS_REBASE_H
#include <stdint.h>

/* The same numbers as tools/rebase_ir.py: each region 02..07 has SS_SLOT bytes of the block, the ROM follows. */
enum { SS_SLOT = 0x80000, SS_SLOTS = 6, SS_ROM_AT = SS_SLOTS * SS_SLOT, SS_ROM_BYTES = 0x1000000,
       SS_ARENA_BYTES = SS_ROM_AT + SS_ROM_BYTES + 0x10000, SS_BACK_ENTRIES = SS_ARENA_BYTES / SS_SLOT + 1 };

extern uint8_t ss_arena[SS_ARENA_BYTES];

/* Fill the two tables the rewritten code reads. Call before anything touches game memory. */
void ss_rebase_init(void);
/* Where the program finds a GBA address (for code that is not rewritten: the start-up, the platform). */
void *ss_host(uint32_t gba);

#endif
