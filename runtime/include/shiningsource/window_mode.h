#ifndef SHININGSOURCE_WINDOW_MODE_H
#define SHININGSOURCE_WINDOW_MODE_H

#include <string.h>

enum ss_window_mode { SS_WINDOW_WINDOWED, SS_WINDOW_BORDERLESS, SS_WINDOW_FULLSCREEN, SS_WINDOW_MODE_COUNT };

static inline const char *ss_window_mode_name(enum ss_window_mode mode)
{
    switch (mode) {
    case SS_WINDOW_WINDOWED: return "windowed";
    case SS_WINDOW_FULLSCREEN: return "fullscreen";
    default: return "borderless";
    }
}

static inline enum ss_window_mode ss_window_mode_from_settings(const char *mode, const char *legacy)
{
    if (mode) {
        if (!strcmp(mode, "windowed")) return SS_WINDOW_WINDOWED;
        if (!strcmp(mode, "borderless")) return SS_WINDOW_BORDERLESS;
        if (!strcmp(mode, "fullscreen")) return SS_WINDOW_FULLSCREEN;
    }
    /* fullscreen=1 used SDL's desktop fullscreen without an exclusive mode. */
    return legacy && !strcmp(legacy, "0") ? SS_WINDOW_WINDOWED : SS_WINDOW_BORDERLESS;
}

#endif
