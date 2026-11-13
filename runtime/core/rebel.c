/* Rebel mode: the cheats, and the frame's hand-over to the platform's achievements.
 *
 * SS_CHEATS is a list of names separated by commas (the launcher writes it). When it is set and not empty the
 * run is a Rebel run until the program ends, whatever happens later: nothing is sent to RetroAchievements, the
 * achievements go to the Rebel record (runtime/achievements), and the game saves to its own file, so that what a
 * cheat gave never reaches a normal run through the save.
 *
 *   hp        health is refilled every frame and the game's own defeat protection is on
 *   sp        the energy that skills and spells spend is refilled every frame
 *   nodeath   only the defeat protection: damage counts, but health stops at 1
 *   gold      the purse in the bag is kept at the game's own limit for one stack (100,000)
 *
 * Each cheat is a few memory writes once per frame, at the frame boundary, to fields the decompilation
 * documents (docs/progress/identification.md there):
 *   player records           0x03003CE0, 0x14C bytes each; the local one is chosen as the game does it
 *                            (sub_080020F4: 0, or the seat in SIOCNT when linked)
 *   +0x62 / +0xC4            maximum and current health        (RestoreLocalPlayerVitals, sub_0802996C)
 *   +0x60 / +0xC2            maximum and current energy
 *   +0x20                    state, 11 = defeated
 *   +0x30 bit 0x100000       defeat protection: health stops at 1 (EnableLocalPlayerDefeatProtection,
 *                            sub_08015CE0; read by sub_08013BC0 and sub_0801DF8C)
 *   *(u32 *)0x030034E0       the character's shared block (0xCA4 bytes on the game's heap); the bag is 25 records
 *                            of 16 bytes at +0x150: u16 kind, u16 id, u32 amount. Kind 10 is money, at most
 *                            100,000 a stack (SumBagCurrency sub_0805D3D4, TryAddCurrencyToBag sub_08058D98)
 *
 * Cheats never act while an online session is active (ss_net_active): every player has to compute the same
 * game. The run stays a Rebel run all the same.
 *
 * This file is part of the core so that every build has the frame hook; a platform without achievements simply
 * registers nothing.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define U32(address) (*(volatile uint32_t *)(uintptr_t)(address))

enum { CHEAT_HP = 1, CHEAT_SP = 2, CHEAT_NODEATH = 4, CHEAT_GOLD = 8 };
enum { PLAYERS = 0x03003CE0u, PLAYER_BYTES = 0x14C, LINK_FLAGS = 0x030027D0u, REG_SIOCNT = 0x04000128u,
       SHARED_BLOCK = 0x030034E0u, SHARED_BYTES = 0xCA4, BAG = 0x150, BAG_SLOTS = 25, MONEY = 10, PURSE_LIMIT = 100000 };

static int started, rebel, cheats;
static void (*host_frame)(int run_flags);

void ss_flash_set_path(const char *path);       /* core/flash.c */

void ss_rebel_set_host(void (*frame)(int run_flags))
{
    host_frame = frame;
}

/* Is `name` one of the words of the comma-separated list? */
static int listed(const char *list, const char *name)
{
    size_t length = strlen(name);
    while (*list) {
        const char *end = list;
        while (*end && *end != ',')
            end++;
        if ((size_t)(end - list) == length && !memcmp(list, name, length))
            return 1;
        list = *end ? end + 1 : end;
    }
    return 0;
}

/* A Rebel run saves to its own file. The first time it starts from a copy of the normal save, so the player goes
 * on with the same characters; nothing is ever copied back. */
static void use_rebel_save(void)
{
    static const char normal[] = "shiningsource.sav", own[] = "shiningsource_rebel.sav";
    static uint8_t bytes[0x10000];
    static const char part[] = "shiningsource_rebel.sav.part";
    FILE *file = fopen(own, "rb");
    int have = 0;
    if (file) {                         /* a file cut short by a crash during the first copy does not count */
        have = fread(bytes, 1, sizeof bytes, file) == sizeof bytes;
        fclose(file);
    }
    if (!have && (file = fopen(normal, "rb")) != NULL) {
        size_t got = fread(bytes, 1, sizeof bytes, file);
        fclose(file);
        /* written beside it and renamed when whole, so there is never half a save under the real name */
        if (got == sizeof bytes && (file = fopen(part, "wb")) != NULL) {
            int whole = fwrite(bytes, 1, sizeof bytes, file) == sizeof bytes;
            whole = fclose(file) == 0 && whole;
            if (whole) {
                remove(own);
                rename(part, own);
            } else {
                remove(part);
            }
        }
    }
    ss_flash_set_path(own);
}

void ss_rebel_start(void)
{
    const char *list = getenv("SS_CHEATS");
    if (started)
        return;
    started = 1;
    if (!list || !*list)
        return;
    rebel = 1;          /* even if no name is known: the run was asked to cheat */
    cheats = (listed(list, "hp") ? CHEAT_HP : 0) | (listed(list, "sp") ? CHEAT_SP : 0)
           | (listed(list, "nodeath") ? CHEAT_NODEATH : 0) | (listed(list, "gold") ? CHEAT_GOLD : 0);
    use_rebel_save();
    fprintf(stderr, "rebel: cheats \"%s\"; this run counts for the Rebel record only and saves to shiningsource_rebel.sav\n", list);
}

/* Keep one stack of money in the bag at the limit: the first stack there is, else a new one in the first empty
 * record, built as the game builds it (kind and amount; the rest of a money record is not read). */
static void fill_purse(void)
{
    uint32_t shared = U32(SHARED_BLOCK), record, empty = 0;
    int i;
    if (shared < SS_EWRAM || shared > SS_EWRAM + 0x40000 - SHARED_BYTES)
        return;
    for (i = 0; i < BAG_SLOTS; i++) {
        record = shared + BAG + (uint32_t)i * 16;
        if (U16(record) == MONEY) {
            if (U32(record + 4) < PURSE_LIMIT)
                U32(record + 4) = PURSE_LIMIT;
            return;
        }
        if (!U16(record) && !empty)
            empty = record;
    }
    if (empty) {
        memset((void *)(uintptr_t)empty, 0, 16);
        U32(empty + 4) = PURSE_LIMIT;
        U16(empty) = MONEY;
    }
}

static uint32_t protected_player;      /* the player record whose protection bit a cheat set, 0 if none */

static void apply_cheats(void)
{
    uint32_t seat = (U32(LINK_FLAGS) & 2) ? (U32(REG_SIOCNT) >> 4) & 3 : 0;
    uint32_t player = PLAYERS + seat * PLAYER_BYTES;
    uint16_t most = U16(player + 0x62);
    if (!most || most > 0x7FFF)
        return;         /* no character in play: the game zeroes the maxima when it resets the players */
    if (cheats & (CHEAT_HP | CHEAT_NODEATH)) {
        if (!(U32(player + 0x30) & 0x100000))
            protected_player = player;  /* this cheat set the bit: it takes it back when cheats stop */
        U32(player + 0x30) |= 0x100000;
    }
    if ((cheats & CHEAT_HP) && U16(player + 0x20) != 11)
        U16(player + 0xC4) = most;
    if (cheats & CHEAT_SP)
        U16(player + 0xC2) = U16(player + 0x60);
    if (cheats & CHEAT_GOLD)
        fill_purse();
}

void ss_rebel_frame(void)
{
    int online, flags;
    if (!started)
        ss_rebel_start();       /* a build whose entry does not call it: the save file was already chosen */
    /* Online for the cheats is the game's own link running, not only a session the transport has joined: the
     * moments before the host is heard and after it left count too, because every machine must compute alike. */
    online = ss_net_active() || (U32(LINK_FLAGS) & 2);
    flags = (rebel ? SS_RUN_REBEL : 0) | (online ? SS_RUN_ONLINE : 0);
    if (cheats && !online) {
        apply_cheats();
        flags |= SS_RUN_CHEATING;
    } else if (protected_player) {
        U32(protected_player + 0x30) &= ~0x100000u;     /* no protection the other players do not have */
        protected_player = 0;
    }
    if (host_frame)
        host_frame(flags);
}
