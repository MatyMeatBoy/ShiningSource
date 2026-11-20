"""Exercise an incoming lethal hit while the enemy is waiting for a palette."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build, prepared_source


class EnemyDeathSync(unittest.TestCase):
    def test_lethal_message_survives_delayed_enemy_initialization(self):
        receiver = prepared_source('code_080659B0.c')
        actor = prepared_source('code_08017F7C.c')
        start = actor.index('    arg0->f5C = arg0->f1D6 = sp118->f6 * 3;')
        end = actor.index('    local124 = 1;', start)
        init_hp = actor[start:end]
        start = actor.index('    arg0->fC = local10C;', actor.index('void sub_0801801C('))
        restore_spawn_state = actor[start:actor.index('\n', start)]
        start = actor.index('    arg0->f4C = 0;\n    local104 = 0;')
        clear_damage = actor[start:actor.index('    arg0->f54 = local120;', start)]
        start = receiver.index('        case 8: /* remote only: entity id')
        end = receiver.index('        case 9: /* Player or entity hit', start)
        receive_damage = receiver[start:end]
        start = actor.index('u32 sub_08019CE8(u8 *obj)')
        end = actor.index('u32 sub_08019D34', start)
        apply_damage = actor[start:end]

        harness = r'''
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;

struct Enemy {
    u16 f0, f2;
    u8 pad04[8];
    u16 fC;
    u8 pad0e[0x3c];
    u16 f4A;
    u32 f4C;
    u8 pad50[0xc];
    u16 f5C, f5E, f7A, f1D4, f1D6;
} gRxEnts[5];

static u32 gRxEntityMask = 1;
static s32 gUnk_030027D0 = 2;
static int online = 1;
#define T015L_F7A (arg0->f7A)
int ss_net_msg_online(void) { return online; }
int sub_080020F4(void) { return 1; }

static void receive(u32 peer, u32 kind, u32 id)
{
    u32 i, d[] = {id};
    switch (kind) {
@RECEIVE@
    }
}

static void finish_initializing(struct Enemy *arg0)
{
    struct { u16 f4, f6; } stats = {5, 40}, *sp118 = &stats;
    int local10C = 14, local104;
@RESTORE_STATE@
@CLEAR_DAMAGE@
@INIT_HP@
}

@APPLY@

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    struct Enemy *enemy = gRxEnts;
    for (int k = 0; k < 2; ++k) {
        u32 tag = k == 0 ? 8 : 0x2F;
        *enemy = (struct Enemy){.f2 = 42, .fC = 11};
        receive(0, tag, 42);
        CHECK(enemy->f7A & 0x80);
        finish_initializing(enemy);
        CHECK(enemy->fC == 10);
        CHECK(enemy->f4A == 120 && enemy->f4C == 120);
        CHECK(sub_08019CE8((u8 *)enemy) == 1 && enemy->f5C == 0);
    }

    *enemy = (struct Enemy){.f2 = 42, .fC = 11};
    receive(1, 8, 42);
    receive(0, 0x2F, 43);
    finish_initializing(enemy);
    CHECK(enemy->fC == 14 && enemy->f5C == 120 && enemy->f4A == 0 && enemy->f4C == 0);

    online = 0;
    *enemy = (struct Enemy){.f2 = 42, .fC = 11};
    receive(0, 0x2F, 42);
    finish_initializing(enemy);
    CHECK(enemy->fC == 14 && enemy->f5C == 120 && enemy->f4A == 0 && enemy->f4C == 0);

    online = 1;
    gUnk_030027D0 = 0;
    *enemy = (struct Enemy){.f2 = 42, .fC = 11};
    receive(0, 0x2F, 42);
    finish_initializing(enemy);
    CHECK(enemy->fC == 14 && enemy->f5C == 120 && enemy->f4A == 0 && enemy->f4C == 0);
    return 0;
}
'''
        harness = (harness.replace('@RECEIVE@', receive_damage).replace('@APPLY@', apply_damage)
                   .replace('@RESTORE_STATE@', restore_spawn_state).replace('@CLEAR_DAMAGE@', clear_damage)
                   .replace('@INIT_HP@', init_hp))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'enemy_death.c'
            exe = Path(directory) / 'enemy_death.exe'
            source.write_text(harness)
            subprocess.run([build.clang(), '-std=c11', str(source), '-o', str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True)
