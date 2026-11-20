"""Remote SFX policy and the actual packet-presentation attack branches."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]


def prepared_source(name='code_0800CF58.c'):
    source = (ROOT / 'games/ss2/decomp/src' / name).read_text()
    patches = json.loads((ROOT / 'games/ss2/port/patches.json').read_text())
    for patch in patches.get(name, []):
        if source.count(patch['find']) != 1:
            raise AssertionError('Port patch no longer applies once: ' + patch['why'])
        source = source.replace(patch['find'], patch['replace'])
    return source


def presentation_branches():
    source = prepared_source()
    case = source.index('            case 2:                                 /* switch 19 */')
    start = source.index('                if ((((struct PlayerD368 *)arg0)->fB0', case)
    end = source.index('                    temp_r0_133 =', start)
    normal = source[start:end] + '\n}\n'
    start = source.index('                if (d368_sub_0800CCCC() != 0)', end)
    end = source.index('            case 5:                                 /* switch 19 */', start)
    bow = source[start:end]
    return normal, bow


def charged_sound_guard():
    source = prepared_source()
    call = source.index('d368_sub_0805865C(0x31);')
    start = source.rfind('                    if (', 0, call)
    end = source.index('}', call) + 1
    return source[start:end]


def pickup_cases():
    source = prepared_source('code_080659B0.c')
    start = source.index('        case 0x19:')
    end = source.index('        case 0x1B:', start)
    return source[start:end]


def boss_contact_case():
    source = prepared_source('code_080659B0.c')
    match = source.index('        case 0xB1 - 0x81:')
    return source[match:source.index('        default:', match)]


def extra_sound_guards():
    """Extract the actual five one-shot sound guards from remote task bodies."""
    requests = [
        ('code_08073E2C.c', 'sub_0805865C(0x17);'),
        ('code_08073E2C.c', 'sub_0805865C(n + 0x100);'),
        ('code_08073E2C.c', 'sub_0805865C(0x1D);'),
        ('code_080153A4.c', 'sub_0805865C(level + 0x100);'),
        ('code_0805A630.c', 'sub_0805865C(0x17);'),
    ]
    guards = []
    for name, call in requests:
        source = prepared_source(name)
        at = source.index(call)
        start = source.rfind('    if (', 0, at)
        guard = source[start:at + len(call)]
        guards.append(guard)
    return guards


HARNESS = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shiningsource/runtime.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
#define M2C_FIELD(base, type, offset) (*(type)((u8 *)(base) + (offset)))
struct PlayerD368 {
    u8 pad0[0xC]; u16 fC; u8 padE[2]; u16 f10; u8 pad12[0xE];
    u16 f20; u8 pad22[0x42]; u16 f64; u8 pad66[0x4A];
    u16 fB0, fB2, fB4, fB6, fB8;
};
_Static_assert(offsetof(struct PlayerD368, f20) == 0x20, "state offset");
_Static_assert(offsetof(struct PlayerD368, fB8) == 0xB8, "animation offset");
struct Actor { struct PlayerD368 player; u32 action; };
static int online, world, map, songs[64], played;
static u16 d368_gUnk_03005CF4[3];
static struct { int second[16]; } animationRows343D368[10];
int ss_net_active(void) { return online; }
static int d368_sub_0800CCCC(void) { return world; }
static int d368_sub_0800CD34(void) { return map; }
static void d368_sub_0805865C(int song) { songs[played++] = song; }
static void d368_sub_08065158(void *sprite, int row, int a, int b, int c) { }
static void normal_frame(struct Actor *actor) {
    u8 *arg0 = (u8 *)&actor->player;
    _Alignas(4) u8 packet[8] = {2, 0, 0, 0x10};
    void *sp180 = packet;
    int sp10 = 0, sp124 = 0;
    u32 sp114 = actor->action, sp1A4 = 0;
    int temp_r0_129, temp_r0_130, temp_r0_131, temp_r0_132;
    /* The following branch is extracted from the patched game source. */
@NORMAL@
    actor->action = sp114;
}
static void bow_frame(struct Actor *actor) {
    u8 *arg0 = (u8 *)&actor->player;
    /* Keep the original case's break statements valid. */
    do {
@BOW@
    } while (0);
}
static void charged_start(int local) {
    struct PlayerD368 player;
    u8 *arg0 = (u8 *)&player;
    memset(&player, 0, sizeof player);
    player.f20 = local ? 0 : 13;
@CHARGED@
}
int sub_0800CCCC(void) { return world; }
int sub_0800CD34(void) { return map; }
void sub_0805865C(int song) { d368_sub_0805865C(song); }
static u32 scene_record[0x44 / 4];
u8 *gUnk_03005CF4 = (u8 *)scene_record;
int32_t gUnk_030027D0;
static u8 sent_audio[10];
static int sent_audio_count;
int ss_net_msg_send(const void *data, int size) {
    if (size != sizeof sent_audio) return 0;
    memcpy(sent_audio, data, size);
    sent_audio_count++;
    return 1;
}
static void extra_task_start(int kind, int local, int n) {
    struct { int state; } actor = { local ? 0 : 13 };
    void *unused = 0;
    int level = n;
    struct { int state; } *p = (void *)&actor, *player = (void *)&actor;
    switch (kind) {
    case 0:
@EXTRA0@
        break;
    case 1:
@EXTRA1@
        break;
    case 2:
@EXTRA2@
        break;
    case 3:
@EXTRA3@
        break;
    case 4:
@EXTRA4@
        break;
    }
}
static struct Actor make_actor(int local, int weapon, int class_id) {
    struct Actor actor;
    memset(&actor, 0, sizeof actor);
    actor.player.f20 = local ? 0 : 13;
    actor.player.f10 = weapon;
    actor.player.fC = class_id;
    actor.player.fB0 = actor.player.fB8 = 1;
    return actor;
}
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)
static int policy(int expected_default) {
    CHECK(ss_remote_sfx_enabled() == expected_default);
    online = 0;
    CHECK(ss_remote_sfx_allowed(1));
    CHECK(!ss_remote_sfx_allowed(0));
    ss_remote_sfx_set_enabled(1);
    online = 1;
    CHECK(ss_remote_sfx_allowed(0));
    CHECK(!ss_remote_sfx_bow_allowed(0, 0));
    CHECK(ss_remote_sfx_bow_allowed(0, 1));
    CHECK(ss_remote_sfx_bow_allowed(1, 0));
    ss_remote_sfx_set_enabled(0);
    CHECK(!ss_remote_sfx_allowed(0));
    CHECK(ss_remote_sfx_allowed(1));
    return 0;
}
static int normal(void) {
    struct Actor actors[4], enabled_actors[4];
    int i;
    online = 1;
    ss_remote_sfx_set_enabled(1);
    for (i = 0; i < 4; i++) {
        actors[i] = make_actor(i == 0, 1, 0);
        normal_frame(&actors[i]);
    }
    CHECK(played == 4);
    memcpy(enabled_actors, actors, sizeof actors);
    for (i = 0; i < 4; i++) normal_frame(&actors[i]);
    CHECK(played == 4); /* Identical records never restart the attack SFX. */
    played = 0;
    ss_remote_sfx_set_enabled(0);
    for (i = 0; i < 4; i++) {
        actors[i] = make_actor(i == 0, 1, 0);
        normal_frame(&actors[i]);
    }
    CHECK(played == 1);
    CHECK(!memcmp(actors, enabled_actors, sizeof actors));
    ss_remote_sfx_set_enabled(1);
    played = 0;
    actors[0] = make_actor(0, 8, 5);
    normal_frame(&actors[0]);
    CHECK(played == 2 && songs[0] == 0x1C && songs[1] == 0x1B);
    normal_frame(&actors[0]);
    CHECK(played == 2);
    played = 0;
    actors[0] = make_actor(0, 8, 7);
    normal_frame(&actors[0]);
    CHECK(played == 2 && songs[0] == 0x1D && songs[1] == 0x1B);
    world = 1;
    actors[0] = make_actor(0, 1, 0);
    normal_frame(&actors[0]);
    CHECK(played == 2);
    world = 0; map = 1;
    actors[0] = make_actor(0, 1, 0);
    normal_frame(&actors[0]);
    CHECK(played == 2);
    return 0;
}
static int bow(void) {
    struct Actor remote = make_actor(0, 4, 3), local = make_actor(1, 4, 3);
    struct Actor before;
    online = 1;
    ss_remote_sfx_set_enabled(1);
    remote.player.fB2 = local.player.fB2 = 1;
    before = remote;
    d368_gUnk_03005CF4[2] = 0x200; /* Frozen animation at its sound cue. */
    bow_frame(&remote);
    bow_frame(&remote);
    CHECK(played == 0);
    CHECK(!memcmp(&remote, &before, sizeof remote));
    bow_frame(&local); /* The original local branch remains unchanged. */
    CHECK(played == 1 && songs[0] == 0x19);
    d368_gUnk_03005CF4[2] = 0;
    bow_frame(&remote);
    CHECK(played == 2 && songs[1] == 0x19);
    remote.player.fB4 = 1; /* Next normal animation tick leaves the cue. */
    bow_frame(&remote);
    CHECK(played == 2);
    remote.player.fB4 = 0;
    ss_remote_sfx_set_enabled(0);
    bow_frame(&remote);
    CHECK(played == 2);
    return 0;
}
static int charged(void) {
    int actor;
    online = 1;
    ss_remote_sfx_set_enabled(1);
    for (actor = 0; actor < 4; actor++) charged_start(actor == 0);
    CHECK(played == 4);
    CHECK(songs[0] == 0x31 && songs[3] == 0x31);
    played = 0;
    ss_remote_sfx_set_enabled(0);
    for (actor = 0; actor < 4; actor++) charged_start(actor == 0);
    CHECK(played == 1);
    ss_remote_sfx_set_enabled(1);
    world = 1;
    charged_start(0);
    CHECK(played == 1);
    world = 0; map = 1;
    charged_start(0);
    CHECK(played == 1);
    return 0;
}
static int extra_tasks(void) {
    int actor, kind;
    online = 1;
    ss_remote_sfx_set_enabled(1);
    for (actor = 0; actor < 4; actor++)
        for (kind = 0; kind < 5; kind++) extra_task_start(kind, actor == 0, 2);
    CHECK(played == 20); /* One initial sound per task and actor. */
    CHECK(songs[0] == 0x17 && songs[1] == 0x102 && songs[2] == 0x1D);
    CHECK(songs[3] == 0x102 && songs[4] == 0x17);
    played = 0;
    extra_task_start(1, 0, 0); /* Preserve the piercing projectile's n > 0. */
    CHECK(played == 0);
    ss_remote_sfx_set_enabled(0);
    for (actor = 0; actor < 4; actor++)
        for (kind = 0; kind < 5; kind++) extra_task_start(kind, actor == 0, 2);
    CHECK(played == 5);
    ss_remote_sfx_set_enabled(1);
    online = 0;
    for (kind = 0; kind < 5; kind++) extra_task_start(kind, 0, 2);
    CHECK(played == 5);
    online = 1; world = 1;
    for (kind = 0; kind < 5; kind++) extra_task_start(kind, 0, 2);
    CHECK(played == 5);
    world = 0; map = 1;
    for (kind = 0; kind < 5; kind++) extra_task_start(kind, 0, 2);
    CHECK(played == 5);
    return 0;
}
static _Alignas(4) u8 pickup_record[32];
static _Alignas(4) u8 pickup_queue[0x88 * 2];
static u8 *gRxQueue = pickup_queue;
static unsigned gRxQueueCount, redrops;
static struct { u32 f30; } gRxPlayers[4];
static int inventory_accepts;
static u8 *x_sub_0805D498(u16 id) {
    return id == *(u16 *)(pickup_record + 12) && *(u16 *)(pickup_record + 10) == 2
        ? pickup_record : NULL;
}
static u8 *x_sub_0805D460(u16 id) {
    return id == *(u16 *)(pickup_record + 12) && *(u16 *)(pickup_record + 10) == 1
        ? pickup_record : NULL;
}
static int x_sub_0805AB20(u8 *record) { return inventory_accepts; }
static void x_sub_0805865C(int song) { sub_0805865C(song); }
static void x_sub_08066490(u32 x, u32 y, u8 *item) { redrops++; }
static int x_sub_0805D4C4(u16 id) {
    if (id != *(u16 *)(pickup_record + 12)) return 0;
    *(u16 *)(pickup_record + 10) = 0;
    *(u16 *)(pickup_record + 12) = 0;
    return 1;
}
static void pickup_message(unsigned myId, unsigned peer, unsigned tag, u32 header, u32 item_id) {
    u32 d[] = {header, item_id};
    switch (tag) {
@PICKUP@
    }
}
static void field_item(unsigned id, unsigned state) {
    *(u16 *)(pickup_record + 10) = state;
    *(u16 *)(pickup_record + 12) = id;
}
static int pickup(void) {
    unsigned picker, observer, result, setting;
    u32 removal;
    online = 1;
    for (picker = 0; picker < 2; picker++) {
        observer = 1 - picker;
        for (result = 0; result <= 1; result++) {
            for (setting = 0; setting <= 1; setting++) {
                gRxQueueCount = played = redrops = 0;
                inventory_accepts = result;
                ss_remote_sfx_set_enabled(setting);
                field_item(42, picker == 0 ? 2 : 1);
                pickup_message(picker, 0, 0x19, (picker << 16) | 1, 42);
                CHECK(gRxQueueCount == 1 && gRxQueue[0] == 0x9B);
                CHECK(played == result && (!result || songs[0] == 0x26));
                CHECK(redrops == 1 - result);
                removal = *(u32 *)(gRxQueue + 8);
                CHECK((u16)removal == 42);
                pickup_message(picker, picker, 0x1A, removal, 0);
                CHECK(played == result && *(u16 *)(pickup_record + 12) == 0);
                field_item(42, observer == 0 ? 2 : 1);
                pickup_message(observer, picker, 0x1A, removal, 0);
                CHECK(played == result + (result && setting));
                CHECK(!(result && setting) || songs[1] == 0x26);
                CHECK(*(u16 *)(pickup_record + 12) == 0);
                pickup_message(observer, picker, 0x1A, removal, 0);
                CHECK(played == result + (result && setting));
            }
        }
    }
    played = gRxQueueCount = 0;
    field_item(42, 1);
    pickup_message(1, 0, 0x19, (1 << 16), 42);
    CHECK(!played && !gRxQueueCount); /* The host refused the request. */
    pickup_message(1, 0, 0x1A, 42, 0);
    CHECK(!played && *(u16 *)(pickup_record + 12) == 0); /* Legacy removal. */
    world = 1; /* Field items are also collected in town (map set zero). */
    field_item(42, 1);
    pickup_message(1, 0, 0x1A, 0x10000u | 42, 0);
    CHECK(played == 1 && songs[0] == 0x26);
    world = played = 0;
    online = 0;
    field_item(42, 2);
    inventory_accepts = 1;
    pickup_message(0, 0, 0x19, 1, 42);
    CHECK(played == 1 && songs[0] == 0x26); /* Keep local sound and payload. */
    CHECK(*(u32 *)(gRxQueue + 8) == 42);
    return 0;
}
static void boss_packet(unsigned myId, unsigned peer, unsigned chunks, u32 cue, u32 area) {
    u32 d[] = {cue, area};
    switch (sent_audio[0] - 0x81) {
@BOSS_CONTACT@
    }
}
static int boss_contact(void) {
    u32 payload[2];
    scene_record[0] = 9;
    scene_record[0x40 / 4] = 5;
    gUnk_030027D0 = 2;
    online = 1;
    played = sent_audio_count = 0;
    ss_remote_sfx_set_enabled(0);
    ss_remote_sfx_boss_contact(0xE2);
    CHECK(played == 1 && songs[0] == 0xE2 && sent_audio_count == 1);
    CHECK(sent_audio[0] == 0xB1 && sent_audio[1] == 1);
    memcpy(payload, sent_audio + 2, sizeof payload);
    CHECK(payload[0] == (9u << 16 | 0xE2) && payload[1] == 5);

    boss_packet(0, 1, 1, payload[0], payload[1]); /* Observer opted out. */
    CHECK(played == 1);
    ss_remote_sfx_set_enabled(1);
    boss_packet(0, 0, 1, payload[0], payload[1]); /* Sender echo. */
    boss_packet(0, 1, 0, payload[0], payload[1]); /* Malformed. */
    boss_packet(0, 1, 1, 9u << 16 | 0x24, 5);   /* Unrelated song. */
    boss_packet(0, 1, 1, 8u << 16 | 0xE2, 5);   /* Different map. */
    boss_packet(0, 1, 1, payload[0], 4);        /* Different area. */
    CHECK(played == 1);
    boss_packet(0, 1, 1, payload[0], payload[1]);
    CHECK(played == 2 && songs[1] == 0xE2);
    boss_packet(1, 0, 1, 9u << 16 | 0xBB, 5);
    CHECK(played == 3 && songs[2] == 0xBB);

    gUnk_030027D0 = 0; /* Solo play even if the launcher has an online endpoint. */
    ss_remote_sfx_boss_contact(0xBB);
    CHECK(played == 4 && songs[3] == 0xBB && sent_audio_count == 1);
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "policy")) return policy(atoi(argv[2]));
    if (!strcmp(argv[1], "normal")) return normal();
    if (!strcmp(argv[1], "bow")) return bow();
    if (!strcmp(argv[1], "extra")) return extra_tasks();
    if (!strcmp(argv[1], "charged")) return charged();
    if (!strcmp(argv[1], "pickup")) return pickup();
    if (!strcmp(argv[1], "boss")) return boss_contact();
    return 3;
}
'''


class RemoteSfx(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(cls.tmp.name)
        normal, bow = presentation_branches()
        source = out / 'remote_sfx.c'
        fixture = HARNESS.replace('@NORMAL@', normal).replace('@BOW@', bow)
        fixture = fixture.replace('@CHARGED@', charged_sound_guard()).replace('@PICKUP@', pickup_cases())
        fixture = fixture.replace('@BOSS_CONTACT@', boss_contact_case())
        for index, guard in enumerate(extra_sound_guards()):
            fixture = fixture.replace('@EXTRA%d@' % index, guard)
        source.write_text(fixture)
        cls.exe = out / 'remote_sfx.exe'
        subprocess.run([
            build.clang(), '-std=c11', '-I', str(ROOT / 'runtime/include'),
            str(source), str(ROOT / 'runtime/core/remote_sfx.c'), '-o', str(cls.exe),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case, default=None):
        env = dict(os.environ)
        env.pop('SS_REMOTE_SFX', None)
        if default is not None:
            env['SS_REMOTE_SFX'] = str(default)
        subprocess.run([
            str(self.exe), case, str(1 if default is None else default),
        ], check=True, env=env)

    def test_policy_defaults_on_and_respects_offline_and_disabled(self):
        self.run_case('policy')
        self.run_case('policy', default=0)

    def test_four_actors_deduplicate_packets_and_keep_original_fallthrough(self):
        self.run_case('normal')

    def test_charge_start_retains_local_and_world_guards(self):
        self.run_case('charged')

    def test_remote_projectiles_charge_and_throw_keep_one_shot_guards(self):
        self.run_case('extra')

    def test_remote_bow_freeze_does_not_repeat_and_local_behavior_is_preserved(self):
        self.run_case('bow')

    def test_pickup_is_heard_once_on_each_peer_only_after_success(self):
        self.run_case('pickup')

    def test_boss_contacts_use_the_current_scene_and_skip_the_sender_echo(self):
        self.run_case('boss')


class RemoteSfxCallbackBoundary(unittest.TestCase):
    def test_option_setter_never_calls_host_services(self):
        # The guest setter is invoked while the host video-init bridge is still
        # active. Calling a bridged getenv/stdio service here reenters the same
        # saved host stack. Substitute real guarded service symbols, so this
        # tests compiled core behavior rather than searching its source text.
        harness = r"""
#include <stdio.h>
#include "shiningsource/runtime.h"
static int callback_active, host_calls;
uint8_t *gUnk_03005CF4;
int32_t gUnk_030027D0;
int ss_net_msg_send(const void *data, int size) { return 0; }
char *guarded_getenv(const char *name) {
    if (callback_active) host_calls++;
    return "1"; /* Enable the logging branch if it accidentally returns. */
}
int guarded_fprintf(FILE *file, const char *format, ...) {
    if (callback_active) host_calls++;
    return 0;
}
int ss_net_active(void) { return 1; }
int sub_0800CCCC(void) { return 0; }
int sub_0800CD34(void) { return 0; }
void sub_0805865C(int song) { if (callback_active) host_calls++; }
int main(void) {
    int selected;
    callback_active = 1;
    ss_remote_sfx_set_enabled(0);
    selected = ss_remote_sfx_enabled();
    callback_active = 0;
    if (host_calls || selected != 0) return 1;
    callback_active = 1;
    ss_remote_sfx_set_enabled(1);
    selected = ss_remote_sfx_enabled();
    callback_active = 0;
    if (host_calls || selected != 1) return 2;
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            source = out / 'setter_boundary.c'
            source.write_text(harness)
            core = out / 'remote_sfx.o'
            subprocess.run([
                build.clang(), '-std=c11', '-Dgetenv=guarded_getenv',
                '-Dfprintf=guarded_fprintf', '-I', str(ROOT / 'runtime/include'),
                '-c', str(ROOT / 'runtime/core/remote_sfx.c'), '-o', str(core),
            ], check=True)
            exe = out / 'setter_boundary.exe'
            subprocess.run([
                build.clang(), '-std=c11', '-I', str(ROOT / 'runtime/include'),
                str(source), str(core), '-o', str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
