/* Entry point of the development build: map the GBA memory, load the ROM, run the game's own main. */
#include "shiningsource/runtime.h"

#include <stdio.h>

void AgbMain(void);

int main(int argc, char **argv)
{
    const char *error = "";

    if (argc < 2) {
        fprintf(stderr, "usage: %s ROM.gba\n", argv[0]);
        return 2;
    }
    if (ss_memory_init(argv[1], &error) != 0) {
        fprintf(stderr, "memory setup failed: %s\n", error);
        return 1;
    }
    fprintf(stderr, "memory mapped, ROM loaded; entering the game's AgbMain\n");
    AgbMain();
    fprintf(stderr, "AgbMain returned; %d distinct stub(s) reached\n", ss_stub_count());
    return 0;
}
