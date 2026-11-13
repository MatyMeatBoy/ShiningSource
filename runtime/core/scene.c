/* Skip scene: let the player jump over the story's scenes, as an option the cartridge did not have.
 *
 * What a scene is, for the Shining Soul engine. Either one of the tasks that only tell the story is running
 * (the introduction, the centred scene messages, the scripted sprite sequences), or a story sequence told in
 * dialogue windows is under way.
 *
 * Story sequences in dialogue windows. The game has no script language for its events: every event is a
 * function that runs as a task, takes the control from the player (the "event lock", bit 11 of the word at
 * 03003478), and opens dialogue windows one after another, each of them a task of its own (sub_0805E0C4) whose
 * record says how the window is left:
 *   a wait of -1: the player dismisses it with A. These are the talks with the people of the towns;
 *   a wait of -2: it ends in a question, yes or no;
 *   a count of frames: it leaves by itself, every page after 141 frames, and A does nothing to it. These are
 *     the scenes of the story (the arrival in a field, the coliseum, the bosses): the game times them so that
 *     with several players every machine tells them in step.
 * So a story sequence is: the event lock is on and a timed window has been shown since it went on. It lasts
 * until the lock goes off. A talk the player started and a shop never show a timed window: they are not scenes,
 * and Start is not taken from them.
 *
 * How a skip works alone: nothing in the game is changed or jumped over. The game simply runs as fast as the
 * machine allows, without picture or sound, until the scene ends; the picture fades out before and in after.
 * So a skipped scene leaves the game exactly where watching it would. A is pressed for the scene's tasks, and
 * in a sequence only while a window that A dismisses is waiting for it. Nothing is ever pressed for a question:
 * the moment a window that asks appears the skip is over, and the player reads and answers it. It is also over
 * when the lock has been on for long without any window (the game waits for something this cannot know: a
 * menu, a name), and nothing is pressed after the lock went off, so no press reaches the game that follows.
 *
 * Alone: Start during a scene asks "skip?"; Start again does it, B or three seconds cancel.
 * Online: Start is a vote. The scene is skipped when every player in the room has voted, and all of them start
 * at the same frame of the shared timeline (core/netlink.c), so the session stays in step. B withdraws the vote.
 * Nothing is run fast online: the game counts its waits for the other players in frames. The introduction is
 * ended by the game's own exit. A story sequence is told by every machine at once, so it is shortened on all of
 * them from the same frame: the timed windows stop waiting (games/ss2/port/patches.json asks ss_skip_talk()),
 * the text and the walks in between keep their speed, and the picture stays, with a small label.
 *
 * SS_SKIP=0 turns it off (Start then reaches the game during scenes, as on the cartridge).
 * Tests: SS_SKIP_AT=N asks and confirms at frame N without a keyboard (also votes, online).
 *        SS_TALK_LOG=1 prints the lock and the dialogue windows whenever they change.
 *        SS_TEST_TALK=N starts, at frame N, a sequence made for the test: a timed window, then a question
 *        (SS_TEST_TALK_KIND=2: a timed window, then one that A dismisses, and the control back at once).
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KEY_A = 0x001, KEY_B = 0x002, KEY_START = 0x008 };
enum { IDLE, PROMPT, VOTING, FADE_OUT, SKIPPING, FADE_IN };
enum { FADE_STEP = 7, PROMPT_FRAMES = 180, SKIP_LIMIT = 36000, ONLINE_LIMIT = 3600 };
enum { SEQUENCE = 4 };          /* the kind of scene that is a story sequence in dialogue windows */
enum { GAP_LIMIT = 900 };       /* frames a sequence may go on without a window before the skip gives up */

uint32_t ss_frame_hash(void);   /* frame.c: a hash of the video state */
void sub_08086860(void);        /* TaskPlayStoryIntroduction */
void sub_08087A5C(void);        /* TaskDisplayCenteredSceneMessage */
void sub_080887D4(void);        /* TaskPlayScriptedSpriteRouteSequence */
void sub_0805E0C4(void);        /* TaskDialogueScriptWindow */

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

/* The introduction is running (alone or with its captions): the one scene the game itself can end at once. */
static int intro_running(void)
{
    uint32_t task = *(const uint32_t *)(uintptr_t)0x03001240u;
    int guard;
    for (guard = 0; task >= SS_EWRAM && task < SS_IWRAM + 0x8000 && guard < 64; guard++) {
        if (*(const uint32_t *)(uintptr_t)(task + 0x0C) == (uint32_t)(uintptr_t)sub_08086860)
            return 1;
        task = *(const uint32_t *)(uintptr_t)task;
    }
    return 0;
}

/* What the dialogue windows are doing. A window is a task of the scheduler's list whose entry is the dialogue
 * task; its record holds the state at +0x74 (0 over, 1 opening, 3 writing, 4 page full, 6 text complete,
 * 7 closing) and at +0x84 how it is left (-1 with A, -2 by answering its question, else frames to wait). */
struct talk {
    int locked;         /* the event lock is on: an event has the control, not the player */
    int windows;        /* dialogue windows on the screen */
    int timed;          /* one of them leaves by itself: the story is being told */
    int wants_a;        /* one of them is left with A and can take the press now */
    int asks;           /* one of them asks the player something */
};

static void talk_look(struct talk *talk)
{
    uint32_t task = *(const uint32_t *)(uintptr_t)0x03001240u;
    int guard;
    memset(talk, 0, sizeof *talk);
    talk->locked = (*(const uint32_t *)(uintptr_t)0x03003478u >> 11) & 1;
    for (guard = 0; task >= SS_EWRAM && task < SS_IWRAM + 0x8000 && guard < 64; guard++) {
        if (*(const uint32_t *)(uintptr_t)(task + 0x0C) == (uint32_t)(uintptr_t)sub_0805E0C4) {
            uint32_t state = *(const uint32_t *)(uintptr_t)(task + 0x74);
            int32_t wait = *(const int32_t *)(uintptr_t)(task + 0x84);
            if (state != 0) {
                talk->windows++;
                if (wait == -2)
                    talk->asks = 1;
                else if (wait != -1)
                    talk->timed = 1;
                else if (state == 3 || state == 4 || state == 6)
                    talk->wants_a = 1;
            }
        }
        task = *(const uint32_t *)(uintptr_t)task;
    }
}

/* Diagnosis, SS_TALK_LOG=1: one line whenever the lock or a dialogue window changes. */
static void talk_log(void)
{
    static int on = -1;
    static uint32_t before[10];
    uint32_t now[10], task = *(const uint32_t *)(uintptr_t)0x03001240u;
    int guard, n = 1, i;
    if (on < 0)
        on = getenv("SS_TALK_LOG") != NULL;
    if (!on)
        return;
    memset(now, 0, sizeof now);
    now[0] = (*(const uint32_t *)(uintptr_t)0x03003478u >> 11) & 1;
    for (guard = 0; task >= SS_EWRAM && task < SS_IWRAM + 0x8000 && guard < 64; guard++) {
        if (*(const uint32_t *)(uintptr_t)(task + 0x0C) == (uint32_t)(uintptr_t)sub_0805E0C4 && n < 9) {
            int32_t wait = *(const int32_t *)(uintptr_t)(task + 0x84);
            now[n++] = *(const uint32_t *)(uintptr_t)(task + 0x74);
            now[n++] = (uint32_t)(wait > 0 ? 1 : wait);         /* a count of frames changes every frame: say only that it is one */
        }
        task = *(const uint32_t *)(uintptr_t)task;
    }
    if (memcmp(now, before, sizeof now)) {
        fprintf(stderr, "talk %6lu: lock %u", ss_frame_number(), (unsigned)now[0]);
        for (i = 1; i < n; i += 2)
            fprintf(stderr, "  [state %u wait %d]", (unsigned)now[i], (int)now[i + 1]);
        fprintf(stderr, "%c", 10);
        memcpy(before, now, sizeof now);
    }
}

/* Test, SS_TEST_TALK=N: at frame N start an event made of the game's own parts, the way its events are made:
 * take the lock, show a timed window, then a window that asks, give the lock back. It says what was answered.
 * With SS_TEST_TALK_KIND=2 the second window is one that A dismisses, and it says when the control came back.
 * (The texts are two of the cartridge's, named by their numbers.) */
void sub_0800D0F4(void);        /* BeginLocalPlayerEventLock */
void sub_0800D108(void);        /* EndLocalPlayerEventLock */
void sub_080003F0(void);        /* wait for the next frame */
uint32_t sub_0805F184(void);    /* IsAnyDialogueTaskSlotUsed */
uint32_t sub_0805DF38(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);       /* StartFixedLayoutDialogueTask */
int32_t sub_0805EEFC(uint32_t); /* DialogueStateIsComplete */
uint32_t sub_0805EF14(uint32_t);        /* DialogueSelectionIsZero: the first choice, yes */
uint32_t sub_08000274(uint32_t, uint32_t, uint32_t);    /* start a task */

static void test_talk_event(void)
{
    uint32_t window;
    sub_0800D0F4();
    while (sub_0805F184() != 0) sub_080003F0();
    window = sub_0805DF38(1, 15, 2, 0, 45, 0x1404E4);
    while (sub_0805EEFC(window) == 0) sub_080003F0();
    while (sub_0805F184() != 0) sub_080003F0();
    if (getenv("SS_TEST_TALK_KIND") && getenv("SS_TEST_TALK_KIND")[0] == '2') {
        window = sub_0805DF38(1, 15, 2, 0, -1, 0x1404FA);
        while (sub_0805EEFC(window) == 0) sub_080003F0();
        sub_0800D108();
        fprintf(stderr, "talk test: the window was dismissed, control back at frame %lu%c", ss_frame_number(), 10);
        return;
    }
    window = sub_0805DF38(1, 15, 2, 0, -2, 0x1404FA);
    while (sub_0805EEFC(window) == 0) sub_080003F0();
    fprintf(stderr, "talk test: the question was answered %s at frame %lu%c", sub_0805EF14(window) ? "yes" : "no", ss_frame_number(), 10);
    sub_0800D108();
}

static void test_talk(void)
{
    static long at = -1;
    static uint32_t record[10];
    if (at < 0)
        at = getenv("SS_TEST_TALK") ? (long)strtoul(getenv("SS_TEST_TALK"), NULL, 10) : 0;
    if (at && ss_frame_number() == (unsigned long)at) {
        memset(record, 0, sizeof record);
        record[2] = 0x50;
        record[3] = (uint32_t)(uintptr_t)test_talk_event;
        fprintf(stderr, "talk test: started, result %u%c", (unsigned)sub_08000274((uint32_t)(uintptr_t)record, 0, 0), 10);
    }
}

/* Asked by the game's introduction where it looks at Start (games/ss2/port/patches.json): the cartridge lets
 * Start end the introduction, but only in a single-player game. While a skip is under way this says "end it
 * now" in any game, so online the scene is ended by the game's own exit, at normal speed, on every player's
 * machine, instead of being run through fast (the game counts its waits for the other players in frames, and
 * run fast they all ran out: communication error). */
static int requested;
int ss_skip_request(void)
{
    return requested;
}

/* Asked by the game's dialogue window where it waits out a timed page (games/ss2/port/patches.json). Online,
 * while the skip every player voted for is under way, this says "do not wait": the window turns its page, or
 * closes, as soon as the text is written. Alone it is never said: there the game is run fast, untouched. */
static int hurried;
int ss_skip_talk(void)
{
    return hurried;
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
    static int story;           /* a story sequence in dialogue windows is under way */
    static int gap;             /* frames of it without any window, while skipping */
    static int shown;           /* online, a sequence: the picture stays while it is hurried */
    static const char *why;     /* tests: what ended the skip */
    static unsigned long frames, test_at = ~0ul, began, ended, last_a;        /* tests: the frames run ahead, and the last press of A among them */
    static uint16_t previous;
    static char line[48];
    uint16_t pressed = (uint16_t)(held & ~previous), game = held;
    int active = ss_scene_active(), online = ss_net_active(), have = 0, need = 0;
    struct talk talk;

    requested = 0;
    hurried = 0;
    talk_log();
    test_talk();
    talk_look(&talk);
    if (!talk.locked)
        story = 0;              /* the player has the control again: the sequence is over */
    else if (talk.timed)
        story = 1;              /* the event shows a window nobody can dismiss: it tells the story */
    if (online)
        active = intro_running();       /* online the introduction is the one scene task skipped: the game can end it itself */
    if (!active && story && !talk.asks)
        active = SEQUENCE;      /* while a question waits there is nothing to skip: Start is the game's again */

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
        if (!active) {
            state = IDLE;                       /* the scene ended, or asks something, while the player made up their mind */
        } else if (pressed & KEY_START) {
            state = FADE_OUT;
            darkness = 0;
        } else if ((pressed & KEY_B) || --timer <= 0) {
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
        shown = online && active != 1;          /* online only the introduction goes dark: a sequence is hurried in full view */
        darkness = shown ? 255 : darkness + FADE_STEP;
        if (darkness >= 255) {
            darkness = shown ? 0 : 255;
            state = SKIPPING;
            timer = 0;
            quiet = 0;
            kind = active;
            still = 0;
            gap = 0;
            began = frames;
            last_a = 0;
            why = "the scene ended";
        }
        view->darkness = darkness;
        break;
    case SKIPPING: {
        /* the same presses on every player: online they follow the shared timeline's frame number */
        unsigned long beat = online ? ss_net_label() : frames;
        /* Online the game reads the buttons once every few frames: a press of two frames in four can fall on
         * the same phase every time and look held for ever. A longer, odd rhythm cannot. */
        int same = active != 0;         /* a story is told by scenes of several kinds, one after another: all of it is skipped */
        if (active)
            kind = active;
        game = shown ? (uint16_t)(held & ~KEY_START) : 0;       /* in full view the player still plays (a window may want their A) */
        if (!online && (beat & 3) < 2 && !talk.asks) {
            /* A scene task takes A at any time. In a sequence A is pressed only while a window that A
             * dismisses can take it: the press that closes the last window is let go on the next frame, long
             * before the lock goes off, and whatever else the event opens (a menu, a list) is never given one. */
            if (active == SEQUENCE ? talk.wants_a : active != 0) {
                game = KEY_A;
                last_a = frames;
            }
        }
        requested = 1;                  /* online nothing is pressed: the game ends the introduction */
        hurried = online && shown;      /* and its timed windows stop waiting */
        quiet = same ? 0 : quiet + 1;
        gap = active == SEQUENCE && !talk.windows ? gap + 1 : 0;
        timer++;
        view->darkness = shown ? 0 : 255;
        view->mute = !shown;
        view->present = shown || timer % 30 == 0;       /* a black frame now and then keeps the window answering */
        view->text = spanish() ? "SALTANDO..." : "SKIPPING...";
        /* Online a player can reach, before the others, a point where the game waits for them (it shows that
         * itself). Nothing changes on the screen then: give the picture back instead of a dark wait. */
        if (online && !shown && (timer & 7) == 0) {
            static uint32_t seen;
            uint32_t now = ss_frame_hash();
            still = now == seen ? still + 8 : 0;
            seen = now;
        }
        if (talk.asks) {                /* the game asks the player something: never answered for them */
            why = "a question waits for the player";
            state = FADE_IN;
        } else if (gap > GAP_LIMIT) {   /* the lock is on and nothing is said: the game waits for something else */
            why = "the game waits without a window";
            state = FADE_IN;
        } else if (quiet >= (kind == SEQUENCE ? 2 : 20) || timer > (online ? ONLINE_LIMIT : SKIP_LIMIT) || (online && still >= 160)) {
            /* (after a sequence the player has the control at once: two frames, not twenty, are run without them) */
            state = FADE_IN;
        }
        if (state == FADE_IN) {
            ended = frames;
            game &= (uint16_t)~KEY_A;
            hurried = 0;
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
                fprintf(stderr, "skip: scene skipped, back at frame %lu (kind %d, run ahead from frame %lu to %lu, last A at %lu, %s)\n",
                        frames, kind, began, ended, last_a, why);
        }
        view->darkness = darkness;
        break;
    }
    (void)voted;
    return game;
}
