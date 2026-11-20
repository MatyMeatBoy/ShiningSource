"""Compile the actual ability audio guards; no ROM or network is needed."""
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_remote_sfx import prepared_source
import build

ROOT = Path(__file__).resolve().parents[2]


def ability_guards():
    source = prepared_source('code_08068670.c')
    pattern = r'if\s*\(([^{};]*?)\)\s*((?:t6f_)?sub_0805865C\([^;]+\);)'
    guards = []
    for match in re.finditer(pattern, source):
        condition, call = match.groups()
        if re.search(r'(?:local|myId|self|sub_080020F4\(\)|t6f_sub_080020F4\(\))\s*==\s*(?:player|owner)', condition):
            guards.append((condition, call))
    return guards


class RestoredAudio(unittest.TestCase):
    def test_self_buff_audio_does_not_spend_remote_resources_or_install_effects(self):
        source = prepared_source('code_08068670.c')
        start = source.index('    case 6:\n', source.index('void sub_0807386C('))
        case = source[start:source.index('    case 8:\n', start)]
        harness = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "shiningsource/runtime.h"
typedef uint8_t u8;typedef int16_t s16;typedef uint16_t u16;typedef uint32_t u32;typedef int32_t s32;
struct Buff07356C {int timer,type,owner,level;};
struct Ply07356C {s16 mp;u16 f26;struct Buff07356C buffs[8];};
static struct Ply07356C players07386C[4];
static u8 *args07686C[4];
static int played,last,visuals;
int ss_net_active(void){return 1;}
int sub_0800CCCC(void){return 0;}
int sub_0800CD34(void){return 0;}
int sub_080020F4(void){return 0;}
int sub_0806898C(int kind,int level){return 3;}
void sub_0805865C(int n){played++;last=n;}
void refresh07686C(void *p){}
void effect07686C(int who,int kind){visuals++;}
static void activate(s32 who){
 struct Ply07356C *player=&players07386C[who];u32 i=0;
 switch(6){
@CASE@
 }
}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 u8 *memory=VirtualAlloc((void *)0x02000000,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 CHECK(memory);
 for(int who=0;who<4;who++){
  args07686C[who]=memory+who*256;players07386C[who].mp=100;players07386C[who].f26=2;
  for(int i=0;i<8;i++)players07386C[who].buffs[i].timer=-1;
 }
 ss_remote_sfx_set_enabled(1);activate(1);
 CHECK(played==1 && last==0x165 && visuals==1);
 CHECK(players07386C[0].mp==100 && players07386C[1].mp==100 && players07386C[1].buffs[0].timer==-1);
 ss_remote_sfx_set_enabled(0);activate(1);
 CHECK(played==1 && players07386C[0].mp==100 && players07386C[1].buffs[0].timer==-1);
 activate(0);CHECK(played==2 && players07386C[0].mp==98 && players07386C[0].buffs[0].type==6);
 return 0;
}
'''.replace('@CASE@', case)
        self.compile_run(harness)

    def test_companion_hurt_and_death_transitions_do_not_repeat(self):
        source = prepared_source('code_0800CF58.c')
        start = source.index('            case 10:                                /* switch 19 */')
        hurt = source[source.index('                if (', start):source.index('                    temp_r0_152 =', start)] + '\n}\n'
        start = source.index('            case 11:                                /* switch 19 */')
        death = source[source.index('                if (', start):source.index('            case 12:', start)]
        harness = r'''
#include <stdint.h>
#include <stdio.h>
#include "shiningsource/runtime.h"
typedef uint8_t u8;typedef uint16_t u16;typedef int16_t s16;typedef uint32_t u32;
#define M2C_FIELD(base,type,offset) (*(type)((u8 *)(base)+(offset)))
struct PlayerD368 {u8 pad0[12];u16 fC;u8 padE[2];u16 f10;u8 pad12[14];u16 f20;u8 pad22[14];u32 f30;u8 pad34[48];u16 f64;u8 pad66[74];u16 fB0,fB2,fB4,fB6,fB8;};
static int online=1,world,map,played,last,action;
static int animationRows338D368[9][10];
int ss_net_active(void){return online;}
int sub_0800CCCC(void){return world;}
int sub_0800CD34(void){return map;}
void sub_0805865C(int n){played++;last=n;}
void d368_sub_08065158(void *a,int b,int c,int d,int e){}
static void tick(struct PlayerD368 *p,int kind){
 u8 *arg0=(u8 *)p,packet[8]={0};void *sp180=packet;
 u32 sp114=action;int sp10,sp124,temp_r0_151,temp_r0_155;
 packet[0]=kind;
 switch(kind){case 10:
@HURT@
break;
case 11:
@DEATH@
 }
 action=sp114;
}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 struct PlayerD368 p={0};p.f20=13;p.fC=2;
 ss_remote_sfx_set_enabled(1);
 tick(&p,10);CHECK(played==1 && last==0x47);
 tick(&p,10);CHECK(played==1);
 tick(&p,11);CHECK(played==2 && last==0x4F);
 tick(&p,11);CHECK(played==2);
 action=0;p.fC=8;tick(&p,10);CHECK(played==3 && last==0x4B);
 tick(&p,11);CHECK(played==4 && last==0x53);
 ss_remote_sfx_set_enabled(0);action=0;tick(&p,10);CHECK(played==4);
 ss_remote_sfx_set_enabled(1);p.f20=0;action=0;tick(&p,10);CHECK(played==4);
 p.f20=13;online=0;action=0;tick(&p,10);CHECK(played==4);
 online=1;world=1;action=0;tick(&p,10);CHECK(played==4);
 return 0;
}
'''.replace('@HURT@', hurt).replace('@DEATH@', death)
        self.compile_run(harness)

    def test_accepted_peer_hits_and_critical_markers_sound_once(self):
        source = prepared_source('code_080659B0.c')
        hit = source[source.index('        case 9: /* Player or entity hit,'):source.index('        case 10:', source.index('        case 9: /* Player or entity hit,'))]
        critical = source[source.index('        case 0x20:\n'):source.index('        case 0x21:\n')]
        objects = source[source.index('        case 0x13: /* damage list:'):source.index('        case 0x14: /* Host spawn,')]
        harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "shiningsource/runtime.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
struct Enemy {u16 f0,f2,fC,f58,f4A,f4C,f62,f226,f5A,f8A; struct {u8 b1;} f1FC;};
struct Player {u16 f88,f20; u32 f110; u8 f6D;};
static struct Enemy enemies[5], *gRxEnts=enemies;
static struct Player gRxPlayers[4];
static u32 gRxEntityMask=1;
static struct ObjectGS {u8 pad[0x59C]; struct {u16 hp,f59E,state;u8 tail[30];} slot[4];} objectGS, *gRxGs=&objectGS;
static u8 *gRxGameState=(u8 *)&objectGS;
static int played, last, online=1, world, map;
int ss_net_active(void) {return online;}
int sub_0800CCCC(void) {return world;}
int sub_0800CD34(void) {return map;}
int sub_080020F4(void) {return 0;}
void sub_0805865C(int n) {played++;last=n;}
void x_sub_0801F2CC(void *a,void *b){}
void x_sub_0801F320(void *a,void *b){}
void *x_sub_0801E244(void *a,int b){return 0;}
void *x_sub_0801E104(void *a,int b){return 0;}
void *x_sub_0801E1A4(void *a,int b){return 0;}
void *x_sub_0801E164(void *a,int b){return 0;}
void x_sub_0801DB94(int a,void *b,int c){}
void x_sub_08021E44(int a,int b){}
u32 x_sub_080008C8(void){return 1;}
static void receive(int tag,u32 peer,u32 *d) {
 u32 i,j,myId=0;
 switch(tag) {
@HIT@
@CRITICAL@
@OBJECTS@
 }
}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 u32 d[2]={4,7};
 enemies[0].f2=4;
 ss_remote_sfx_set_enabled(1);
 receive(9,1,d);CHECK(played==1 && last==0x1A && enemies[0].f4A==7);
 receive(9,0,d);CHECK(played==1); /* own echo */
 d[0]=99;receive(9,1,d);CHECK(played==1); /* stale entity */
 d[0]=4;d[1]=0;receive(9,1,d);CHECK(played==1); /* zero damage */
 d[1]=7;ss_remote_sfx_set_enabled(0);receive(9,1,d);CHECK(played==1 && enemies[0].f4A==14);
 ss_remote_sfx_set_enabled(1);world=1;receive(9,1,d);CHECK(played==1);world=0;
 map=1;receive(9,1,d);CHECK(played==1);map=0;
 d[0]=4;receive(0x20,1,d);CHECK(played==2 && last==0x132);
 receive(0x20,0,d);CHECK(played==2);
 d[0]=99;receive(0x20,1,d);CHECK(played==2);
 online=0;d[0]=4;receive(9,1,d);CHECK(played==2);
 online=1;
 {u8 record[8]={0,0,0,0,0,0,0,1};const u8 *ptr=record;
  *(u32 *)(gRxGameState+0x48)=1;objectGS.slot[0].hp=100;
  memcpy((u8 *)&objectGS.slot[0]+0x10,&ptr,sizeof ptr);
  d[0]=1;d[1]=5;receive(0x13,1,d);CHECK(played==3 && last==0x2A && objectGS.slot[0].hp==95);
  receive(0x13,0,d);CHECK(played==3 && objectGS.slot[0].hp==95);
  objectGS.slot[0].state=0xB;receive(0x13,1,d);CHECK(played==3);
  objectGS.slot[0].state=0;d[1]=0;receive(0x13,1,d);CHECK(played==3);
 }
 return 0;
}
'''.replace('@HIT@', hit).replace('@CRITICAL@', critical).replace('@OBJECTS@', objects)
        self.compile_run(harness)

    def test_all_remote_ability_audio_gates_keep_world_and_phase_guards(self):
        guards = ability_guards()
        self.assertEqual(len(guards), 22)
        harness = r'''
#include <stdio.h>
#include "shiningsource/runtime.h"
static int online, world, map, played, song;
int ss_net_active(void) { return online; }
int sub_0800CCCC(void) { return world; }
int sub_0800CD34(void) { return map; }
int sub_080020F4(void) { return 0; }
void sub_0805865C(int n) { played++; song=n; }
#define t6f_sub_0800CCCC sub_0800CCCC
#define t6f_sub_0800CD34 sub_0800CD34
#define t6f_sub_080020F4 sub_080020F4
#define t6f_sub_0805865C sub_0805865C
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"guard %d line %d: %s\n", gate,__LINE__,#c); return 1; } } while(0)
@FUNCTIONS@
int main(void) {
    int gate;
    void (*calls[])(int,int)={@CALLS@};
    for(gate=0;gate<sizeof(calls)/sizeof(calls[0]);gate++) {
        online=1; world=map=0; ss_remote_sfx_set_enabled(1); played=0;
        calls[gate](1,0); CHECK(played==1);
        played=0; calls[gate](1,1); CHECK(played==@PHASE_EXPECTED@);
        ss_remote_sfx_set_enabled(0); played=0; calls[gate](1,0); CHECK(played==0);
        calls[gate](0,0); CHECK(played==1); /* local unchanged when disabled */
        online=0; ss_remote_sfx_set_enabled(1); played=0; calls[gate](1,0); CHECK(played==0);
        online=1; world=1; calls[gate](1,0); CHECK(played==0);
        world=0; map=1; calls[gate](1,0); CHECK(played==0);
        map=0;
    }
    return 0;
}
'''
        functions = []
        for index, (condition, call) in enumerate(guards):
            functions.append('static void guard%d(int actor, int clock) {\n'
                             'int local=0,myId=0,self=0,player=actor,owner=actor,level=2,tmo=2;\n'
                             'int frame[1]={1},phase[1]={clock},i=0;\n'
                             'struct {int level;} arg={2}, *arg0=&arg;\n'
                             'if (%s) %s\n}\n' % (index, condition, call))
        phase_expected = '{' + ','.join('0' if 'phase[i]' in condition else '1' for condition,call in guards) + '}'
        harness = harness.replace('@FUNCTIONS@', '\n'.join(functions)).replace('@CALLS@', ','.join('guard%d'%i for i in range(len(guards))))
        harness = harness.replace('    int gate;', '    int gate; int phase_expected[]='+phase_expected+';').replace('@PHASE_EXPECTED@', 'phase_expected[gate]')
        self.compile_run(harness)

    def compile_run(self, harness):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'audio.c'
            exe = Path(directory)/'audio.exe'
            source.write_text(harness + r'''
uint8_t *gUnk_03005CF4;
int32_t gUnk_030027D0;
int ss_net_msg_send(const void *data, int size) { return 0; }
''')
            subprocess.run([build.clang(), '-std=c11', '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-I', str(ROOT/'runtime/include'), str(source),
                            str(ROOT/'runtime/core/remote_sfx.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
