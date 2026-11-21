#ifndef SHININGSOURCE_COUCH_LAYOUT_H
#define SHININGSOURCE_COUCH_LAYOUT_H

#include "shiningsource/runtime.h"

enum { SS_COUCH_MAX_PLAYERS = 4 };

struct ss_couch_rect {
    int x, y, scale, view;
};

/* Give every player the same whole-number scale. Among layouts with the same
 * scale, Enhanced uses the one that shows the most world per player. */
static inline void ss_couch_layout(int w, int h, int players, int enhanced, struct ss_couch_rect *seats)
{
    const int gap = 2;
    int best_cols = 1, best_rows = 1, best_scale = 0, best_view = 0;
    for (int cols = 1; cols <= players; ++cols) {
        int rows = (players + cols - 1) / cols;
        int cw = (w - gap * (cols - 1)) / cols;
        int ch = (h - gap * (rows - 1)) / rows;
        int scale = cw / SS_SCREEN_W;
        if (scale > ch / SS_SCREEN_H)
            scale = ch / SS_SCREEN_H;
        if (scale < 1)
            continue;
        int view = enhanced ? cw / scale : SS_SCREEN_W;
        if (view > SS_MAX_VIEW_W)
            view = SS_MAX_VIEW_W;
        if (scale > best_scale || (scale == best_scale && view > best_view) ||
            (scale == best_scale && view == best_view && cols == 2)) {
            best_cols = cols;
            best_rows = rows;
            best_scale = scale;
            best_view = view;
        }
    }
    int cell_h = (h - gap * (best_rows - 1)) / best_rows;
    for (int i = 0; i < players; ++i) {
        int row = i / best_cols, col = i % best_cols;
        int cols = players - row * best_cols;
        if (cols > best_cols)
            cols = best_cols;
        int cell_w = (w - gap * (cols - 1)) / cols;
        int view = enhanced && best_scale ? cell_w / best_scale : best_view;
        if (view > SS_MAX_VIEW_W)
            view = SS_MAX_VIEW_W;
        int row_width = cols * cell_w + (cols - 1) * gap;
        seats[i].x = (w - row_width) / 2 + col * (cell_w + gap) + (cell_w - view * best_scale) / 2;
        seats[i].y = row * (cell_h + gap) + (cell_h - SS_SCREEN_H * best_scale) / 2;
        seats[i].scale = best_scale;
        seats[i].view = view;
    }
}

struct ss_couch_select {
    uint64_t first;
    unsigned presses;
};

static inline int ss_couch_select_press(struct ss_couch_select *state, uint64_t now)
{
    if (!state->presses || now - state->first > 800) {
        state->presses = 0;
        state->first = now;
    }
    if (++state->presses < 3)
        return 0;
    state->presses = 0;
    return 1;
}

#endif
