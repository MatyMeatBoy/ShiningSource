/* Experimental remote combat sound presentation. The game already receives
 * other actors' actions; only its explicit remote-state sound exclusions change.
 * No audio stream, packet or gameplay state is added. */
#include "shiningsource/runtime.h"
#include <stdlib.h>

static int enabled = -1;

int ss_remote_sfx_enabled(void)
{
    if (enabled < 0) {
        const char *text = getenv("SS_REMOTE_SFX");
        enabled = !(text && text[0] == '0');
    }
    return enabled;
}

void ss_remote_sfx_set_enabled(int on)
{
    /* Called back by the options host on its native service stack: stay a leaf
     * operation. Calling getenv/write/etc here would re-enter the host bridge. */
    enabled = on != 0;
}

int ss_remote_sfx_allowed(int original_local)
{
    return original_local || (ss_remote_sfx_enabled() && ss_net_active());
}

int ss_remote_sfx_bow_allowed(int original_local, int clock_running)
{
    /* Remote release is checked each animation tick. Waiting through a frozen
     * clock prevents a zero-phase release from sounding on every frozen frame.
     * Preserve the original local-player branch, including its timing. */
    return original_local || (clock_running && ss_remote_sfx_allowed(0));
}
