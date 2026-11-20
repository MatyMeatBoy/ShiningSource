/* Restore audio for replicated actions and confirmed effects that the original
 * game played only to the player involved. Use the original song IDs. */
#include "shiningsource/runtime.h"
#include <stdlib.h>
#include <string.h>

extern int sub_0800CCCC(void);
extern int sub_0800CD34(void);
extern void sub_0805865C(int song);
extern int32_t gUnk_030027D0;
extern uint8_t *gUnk_03005CF4;

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

/* Only call from an accepted remote presentation event, never from packet polling.
 * The sender already played its local cue. Keep the game's scene/audio guards. */
void ss_remote_sfx_event(int song)
{
    if (ss_remote_sfx_allowed(0) && !sub_0800CCCC() && !sub_0800CD34())
        sub_0805865C(song);
}

static uint16_t boss_scene_map(void)
{
    uint16_t map;
    memcpy(&map, gUnk_03005CF4, sizeof map);
    return map;
}

static uint32_t boss_scene_area(void)
{
    uint32_t area;
    memcpy(&area, gUnk_03005CF4 + 0x40, sizeof area);
    return area;
}

void ss_remote_sfx_boss_contact(int song)
{
    uint8_t message[10] = { 0xB1, 1 };
    uint32_t payload[2];

    sub_0805865C(song);
    if (!(gUnk_030027D0 & 2) || (gUnk_030027D0 & 0x20) ||
        !ss_net_active() || !gUnk_03005CF4)
        return;
    payload[0] = (uint32_t)boss_scene_map() << 16 | (uint16_t)song;
    payload[1] = boss_scene_area();
    memcpy(message + 2, payload, sizeof payload);
    /* A cosmetic cue can be dropped when the ordered channel is full.
     * Never delay a hit, damage task or scene transition to send it. */
    ss_net_msg_send(message, sizeof message);
}

void ss_remote_sfx_boss_receive(uint32_t cue, uint32_t area)
{
    uint16_t song = (uint16_t)cue;

    if ((song == 0xBB || song == 0xE2) && gUnk_03005CF4 &&
        (cue >> 16) == boss_scene_map() && area == boss_scene_area())
        ss_remote_sfx_event(song);
}
