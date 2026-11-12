/* Bookkeeping for functions that are not implemented yet. */
#include "shiningsource/runtime.h"

#include <stdio.h>

static int reached;

void ss_stub(const char *name, int *seen)
{
    if (*seen)
        return;
    *seen = 1;
    reached++;
    fprintf(stderr, "[stub %3d] %s\n", reached, name);
    fflush(stderr);
}

int ss_stub_count(void)
{
    return reached;
}
