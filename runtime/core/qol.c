/* Quality of life, experimental: buttons the console never had.
 *
 * The game only knows the console's ten buttons. The platform (or a test script) may report two more, above
 * them in the same mask: SS_KEY_L_BACK and SS_KEY_R_BACK, "step the L bar backwards" and "step the R bar
 * backwards" (a controller's triggers, see ss_plat_present). They are turned into things the game understands:
 *
 *   - the game is given the bar's own button (L or R) for as long as the extra one is held, so everything that
 *     button does still happens exactly as the game does it: the bar opens, its timer restarts, the sound plays,
 *     the same conditions forbid it;
 *   - and a flag says that the step this press causes goes the other way. The game asks for it at the one place
 *     where it moves the selection (games/<game>/port/patches.json), and there steps forwards (slots - 1) times
 *     instead of once. The result is a state the game can reach by itself, so nothing new has to be saved or
 *     sent to the other players.
 *
 * The flag is raised when the extra button starts giving the bar's button and stays up until the player holds
 * the real button, which is the only other way to cause a step. So it does not matter in which frame the game
 * notices the press.
 *
 * Who decides whether the option is on: the platform for its own buttons (it reports them only when on, see
 * ss_qol_set_bars) and SS_QOL_BARS=1 for a test script (ss_qol_scripted). With the option off, or no extra
 * button held, ss_qol_keys returns what it was given and the flags stay down: the game runs as it always did.
 */
#include "shiningsource/runtime.h"

#include <stdlib.h>

enum { KEY_R = 0x0100, KEY_L = 0x0200 };

static int back[2];             /* the next step of the L bar (0) or the R bar (1) goes backwards */

/* The extra buttons of a test script's mask, or none when SS_QOL_BARS is not 1. */
uint16_t ss_qol_scripted(unsigned long mask)
{
    static int on = -1;
    if (on < 0) {
        const char *text = getenv("SS_QOL_BARS");
        on = text && text[0] == '1';
    }
    return on ? (uint16_t)(mask & (SS_KEY_L_BACK | SS_KEY_R_BACK)) : 0;
}

/* Once per frame: the buttons held, extra ones included; returns the console's buttons the game is given. */
uint16_t ss_qol_keys(uint16_t held)
{
    static const uint16_t extra[2] = { SS_KEY_L_BACK, SS_KEY_R_BACK }, key[2] = { KEY_L, KEY_R };
    uint16_t given = held & 0x03FF;
    int bar;
    for (bar = 0; bar < 2; bar++) {
        if (held & key[bar])
            back[bar] = 0;                          /* the real button: forwards, as in the game */
        else if (held & extra[bar]) {
            given |= key[bar];
            back[bar] = 1;
        }
    }
    return given;
}

/* Asked by the game where it steps a bar: 1 if this step is to go backwards. */
int ss_qol_bar_back(int bar)
{
    return back[bar & 1];
}
