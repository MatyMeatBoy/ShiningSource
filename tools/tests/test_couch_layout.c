#include <assert.h>
#include "shiningsource/couch_layout.h"

static void check(int w, int h, int players, int enhanced, int scale, int width)
{
    struct ss_couch_rect seats[SS_COUCH_MAX_PLAYERS];
    ss_couch_layout(w, h, players, enhanced, seats);
    for (int i = 0; i < players; ++i) {
        int expected_width = players == 3 && enhanced && i == 2 ? 480 : width;
        assert(seats[i].scale == scale);
        assert(seats[i].view == expected_width);
        assert(seats[i].x >= 0 && seats[i].y >= 0);
        assert(seats[i].x + scale * expected_width <= w);
        assert(seats[i].y + scale * 160 <= h);
        for (int j = 0; j < i; ++j)
            assert(seats[i].x >= seats[j].x + scale * seats[j].view ||
                   seats[j].x >= seats[i].x + scale * expected_width ||
                   seats[i].y >= seats[j].y + scale * 160 ||
                   seats[j].y >= seats[i].y + scale * 160);
    }
}

int main(void)
{
    check(1920, 1080, 2, 0, 3, 240);
    check(1920, 1080, 2, 1, 3, 480);
    check(1920, 1080, 3, 1, 3, 319);
    check(1920, 1080, 4, 0, 3, 240);
    check(1280, 720, 2, 1, 2, 480);
    check(1280, 720, 4, 1, 2, 319);
    check(1024, 768, 2, 0, 2, 240);
    check(1024, 768, 3, 1, 2, 255);
    check(1600, 1200, 4, 1, 3, 266);
    check(720, 1080, 2, 0, 3, 240);
    struct ss_couch_select a = {0}, b = {0};
    assert(!ss_couch_select_press(&a, 10));
    assert(!ss_couch_select_press(&a, 330));
    assert(!ss_couch_select_press(&b, 500));
    assert(ss_couch_select_press(&a, 773));
    assert(!ss_couch_select_press(&b, 1301));
    assert(!ss_couch_select_press(&a, 780));
    assert(!ss_couch_select_press(&a, 1550));
    return 0;
}
