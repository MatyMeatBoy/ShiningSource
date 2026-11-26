/* The song table runtime/core/remote_sfx.c reads, for the fixtures that link it without the game: every song is
 * on music player 2 (sound effects) except the empty songs 0x4D-0x54 and song 1 (area music), on player 0, as in
 * the cartridge's table. */
#include <stdint.h>

uint8_t gSongTable[0x1AC * 8];

__attribute__((constructor)) static void song_players(void)
{
    int n;
    for (n = 0; n < 0x1AC; n++)
        gSongTable[n * 8 + 4] = gSongTable[n * 8 + 6] = (n == 1 || (n >= 0x4D && n <= 0x54)) ? 0 : 2;
}
