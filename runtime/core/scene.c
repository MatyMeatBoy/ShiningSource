/* Skip scene: let the player jump over the story's scenes, as an option the cartridge did not have.
 *
 * What a scene is, for the Shining Soul engine: one of the tasks that only tell the story is running (the
 * introduction, the centred scene messages, the scripted sprite sequences). Dialogue windows with a character
 * are not scenes here: many of them ask something, and skipping would answer for the player.
 *
 * How a skip works: nothing in the game is changed or jumped over. The game simply runs as fast as the machine
 * allows, without picture or sound, while the A button is pressed for it, until the scene's task ends; the
 * picture fades out before and in after. So a skipped scene leaves the game exactly where watching it would.
 *
 * Alone: Start during a scene asks "skip?"; Start again does it, B or three seconds cancel.
 * Online: Start is a vote. The scene is skipped when every player in the room has voted, and all of them start
 * at the same frame of the shared timeline (core/netlink.c), so the session stays in step. B withdraws the vote.
 *
 * SS_SKIP=0 turns it off (Start then reaches the game during scenes, as on the cartridge).
 * Tests: SS_SKIP_AT=N asks and confirms at frame N without a keyboard (also votes, online).
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KEY_A = 0x001, KEY_B = 0x002, KEY_START = 0x008 };
enum { IDLE, PROMPT, VOTING, FADE_OUT, SKIPPING, FADE_IN };
enum { FADE_STEP = 13, PROMPT_FRAMES = 180, SKIP_LIMIT = 36000, ONLINE_LIMIT = 3600 };

uint32_t ss_frame_hash(void);   /* frame.c: a hash of the video state */
void sub_08086860(void);        /* TaskPlayStoryIntroduction */
void sub_08087A5C(void);        /* TaskDisplayCenteredSceneMessage */
void sub_080887D4(void);        /* TaskPlayScriptedSpriteRouteSequence */

/* Is a story-telling task in the scheduler's list? Returns which kind (1 introduction, 2 message, 3 sprite sequence). (struct Task: next at +0, entry at +0x0C, see sched.c) */
int ss_scene_active(void)
{
    uint32_t task = *(const uint32_t *)(uintptr_t)0x03001240u;
    int guard;
    if (getenv("SS_TASKS") && ss_frame_number() == strtoul(getenv("SS_TASKS"), NULL, 10)) {   /* diagnosis: who is running */
        uint32_t t = task;
        for (guard = 0; t >= SS_EWRAM && t < SS_IWRAM + 0x8000 && guard < 64; guard++) {
            fprintf(stderr, "task %08X entry %08X state %u%c", (unsigned)t, (unsigned)*(const uint32_t *)(uintptr_t)(t + 0x0C),
                    (unsigned)*(const uint32_t *)(uintptr_t)(t + 4), 10);
            t = *(const uint32_t *)(uintptr_t)t;
        }
    }
    for (guard = 0; task >= SS_EWRAM && task < SS_IWRAM + 0x8000 && guard < 64; guard++) {
        uint32_t entry = *(const uint32_t *)(uintptr_t)(task + 0x0C);
        if (entry == (uint32_t)(uintptr_t)sub_08086860 || entry == (uint32_t)(uintptr_t)sub_08087A5C
            || entry == (uint32_t)(uintptr_t)sub_080887D4)
            return entry == (uint32_t)(uintptr_t)sub_08086860 ? 1 : entry == (uint32_t)(uintptr_t)sub_08087A5C ? 2 : 3;
        task = *(const uint32_t *)(uintptr_t)task;
    }
    return 0;
}

static int spanish(void)
{
    static int known = -1;
    if (known < 0) {
        const char *text = getenv("SS_LANG");
        known = text && text[0] == 'e' && text[1] == 's';
    }
    return known;
}

/* One frame. `held` are the buttons the player holds; the result are the buttons the game is given. */
uint16_t ss_skip_update(uint16_t held, struct ss_skip_view *view)
{
    static int state = IDLE, timer, darkness, quiet, voted, kind, still;
    static unsigned long frames, test_at = ~0ul;
    static uint16_t previous;
    static char line[48];
    uint16_t pressed = (uint16_t)(held & ~previous), game = held;
    int active = ss_scene_active(), online = ss_net_active(), have = 0, need = 0;

    static int enabled = -1;
    if (enabled < 0)
        enabled = !(getenv("SS_SKIP") && getenv("SS_SKIP")[0] == '0');       /* SS_SKIP=0 turns the feature off */
    if (!enabled) {
        view->darkness = 0; view->text = NULL; view->present = 1; view->mute = 0;
        return held;
    }
    if (test_at == ~0ul)
        test_at = getenv("SS_SKIP_AT") ? strtoul(getenv("SS_SKIP_AT"), NULL, 10) : 0;
    frames++;
    if (test_at && (frames == test_at || (frames == test_at + 2 && !online)))
        pressed |= KEY_START;
    previous = held;
    view->darkness = 0;
    view->text = NULL;
    view->present = 1;
    view->mute = 0;
    if (active)
        game &= (uint16_t)~KEY_START;           /* during a scene Start belongs to this, not to the game */

    switch (state) {
    case IDLE:
        if (online) {
            ss_net_votes(&have, &need);
            if (active && (pressed & KEY_START)) {
                voted = 1;
                ss_net_vote(1);
                state = VOTING;
            } else if (active && have > 0) {
                snprintf(line, sizeof line, spanish() ? "SALTAR ESCENA: %d/%d. START = SI" : "SKIP SCENE: %d/%d. START = YES", have, need);
                view->text = line;
            }
            if (ss_net_skip_now()) {            /* the others agreed while this player had not voted: cannot happen, but stay in step */
                state = FADE_OUT;
                darkness = 0;
            }
        } else if (active && (pressed & KEY_START)) {
            state = PROMPT;
            timer = PROMPT_FRAMES;
        }
        break;
    case PROMPT:
        game &= (uint16_t)~(KEY_B | KEY_START);
        view->text = spanish() ? "SALTAR ESCENA? START = SI  B = NO" : "SKIP SCENE? START = YES  B = NO";
        if (pressed & KEY_START) {
            state = FADE_OUT;
            darkness = 0;
        } else if ((pressed & KEY_B) || --timer <= 0 || !active) {
            state = IDLE;
        }
        break;
    case VOTING:
        game &= (uint16_t)~(KEY_B | KEY_START);
        ss_net_votes(&have, &need);
        snprintf(line, sizeof line, spanish() ? "SALTAR: %d/%d PULSARON START. B = NO" : "SKIP: %d/%d PRESSED START. B = NO", have, need);
        view->text = line;
        if (ss_net_skip_now()) {
            state = FADE_OUT;
            darkness = 0;
            voted = 0;
        } else if ((pressed & KEY_B) || !active || !online) {
            ss_net_vote(0);
            voted = 0;
            state = IDLE;
        }
        break;
    case FADE_OUT:
        game = 0;
        darkness += FADE_STEP;
        if (darkness >= 255) {
            darkness = 255;
            state = SKIPPING;
            timer = 0;
            quiet = 0;
            kind = active;
            still = 0;
        }
        view->darkness = darkness;
        break;
    case SKIPPING: {
        /* the same presses on every player: online they follow the shared timeline's frame number */
        unsigned long beat = online ? ss_net_label() : frames;
        /* Online the game reads the buttons once every few frames: a press of two frames in four can fall on
         * the same phase every time and look held for ever. A longer, odd rhythm cannot. */
        int same = active != 0;         /* a story is told by scenes of several kinds, one after another: all of it is skipped */
        game = same && (online ? beat % 10 < 5 : (beat & 3) < 2) ? KEY_A : 0;
        quiet = same ? 0 : quiet + 1;
        timer++;
        view->darkness = 255;
        view->mute = 1;
        view->present = timer % 30 == 0;        /* a black frame now and then keeps the window answering */
        view->text = spanish() ? "SALTANDO..." : "SKIPPING...";
        /* Online a player can reach, before the others, a point where the game waits for them (it shows that
         * itself). Nothing changes on the screen then: give the picture back instead of a dark wait. */
        if (online && (timer & 7) == 0) {
            static uint32_t seen;
            uint32_t now = ss_frame_hash();
            still = now == seen ? still + 8 : 0;
            seen = now;
        }
        if (quiet >= 20 || timer > (online ? ONLINE_LIMIT : SKIP_LIMIT) || (online && still >= 160)) {
            state = FADE_IN;
            view->present = 1;
        }
        break;
    }
    case FADE_IN:
        darkness -= FADE_STEP;
        if (darkness <= 0) {
            darkness = 0;
            state = IDLE;
            if (getenv("SS_SKIP_AT"))
                fprintf(stderr, "skip: scene skipped, back at frame %lu\n", frames);
        }
        view->darkness = darkness;
        break;
    }
    (void)voted;
    return game;
}
