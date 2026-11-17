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
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "policy")) return policy(atoi(argv[2]));
    if (!strcmp(argv[1], "normal")) return normal();
    if (!strcmp(argv[1], "bow")) return bow();
    if (!strcmp(argv[1], "extra")) return extra_tasks();
    if (!strcmp(argv[1], "charged")) return charged();
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
        fixture = fixture.replace('@CHARGED@', charged_sound_guard())
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
