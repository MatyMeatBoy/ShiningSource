"""Replay every alternative in a downloaded official RA catalog against the actual host.
Usage: python tools/tests/verify_ra_catalog.py CATALOG ROM OUTPUT_DIRECTORY
No account requests or awards are made. Snapshots are synthetic condition transitions,
not evidence that a complete playthrough has been performed.
"""
import json, re, struct, subprocess, sys, shutil
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import build,link
ROOT=Path(__file__).resolve().parents[2]
MEMORY=0x48000

def assign(memory, operand, value):
    match=re.fullmatch(r'0x([ HLMNOPQRSTUX])([0-9a-fA-F]{6})',operand)
    if not match: raise ValueError('Unsupported operand '+operand)
    kind,address=match.groups();address=int(address,16)
    if kind in 'MNOPQRST':
        bit=ord(kind)-ord('M');mask=1<<bit
        memory[address]=(memory.get(address,0)&~mask)|((value&1)<<bit)
    elif kind in 'LU':
        shift=0 if kind=='L' else 4;mask=15<<shift
        memory[address]=(memory.get(address,0)&~mask)|((value&15)<<shift)
    else:
        width={'H':1,' ':2,'X':4}[kind]
        for i in range(width): memory[address+i]=(value>>(i*8))&255

def snapshot(branch):
    current={};previous=[];skip_or=False
    for condition in branch.split('_'):
        is_or=condition.startswith('O:')
        if skip_or:
            if not is_or: skip_or=False
            continue
        if is_or: condition=condition[2:];skip_or=True
        match=re.fullmatch(r'(d?)(0x[ HLMNOPQRSTUX][0-9a-fA-F]{6})(=|>=|>|<|<=)([0-9]+)',condition)
        if not match: raise ValueError('Unsupported condition '+condition)
        delta,operand,op,value=match.groups();value=int(value)
        value += 1 if op=='>' else -1 if op=='<' else 0
        if delta: previous.append((operand,value))
        else: assign(current,operand,value)
    before=dict(current)
    for operand,value in previous: assign(before,operand,value)
    return before,current

HARNESS=r"""
#include "rc_runtime_types.h"
#include "runtime/achievements/ach_host.c"
static unsigned notices, last_size;
void ss_plat_notice(const char *h,const char *t,int r,const uint32_t *p,int s) { notices++; last_size=s; }
static void apply(FILE *f) {
    uint32_t n,address; unsigned char value;
    fread(&n,4,1,f);
    while(n--) { fread(&address,4,1,f); fread(&value,1,1,f); *(uint8_t *)real_address(address)=value; }
}
int main(int argc,char **argv) {
    FILE *f; uint32_t cases,id,index; long size; unsigned expected,passed=0,setup_unlocks=0;
    char record[2200];
    if(argc!=5)return 1;
    snprintf(folder_utf8,sizeof folder_utf8,"%s",argv[1]);
    MultiByteToWideChar(CP_UTF8,0,argv[1],-1,folder,900);
    expected=(unsigned)atoi(argv[4]);
    f=fopen(argv[2],"rb"); fseek(f,0,SEEK_END); size=ftell(f); rewind(f);
    rom_size=(size_t)size;rom_bytes=malloc(rom_size);fread((void *)rom_bytes,1,rom_size,f);fclose(f);
    if(VirtualAlloc((void *)SS_IWRAM,0x8000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE)!=(void *)SS_IWRAM)return 2;
    if(VirtualAlloc((void *)SS_EWRAM,0x40000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE)!=(void *)SS_EWRAM)return 3;
    snprintf(record,sizeof record,"%s/local_record.txt",argv[1]);
    f=fopen(argv[3],"rb");fread(&cases,4,1,f);
    for(index=0;index<cases;index++) {
        struct local_entry *entry; unsigned i;
        DeleteFileA(record);local_total=local_earned=0;local_list=NULL;notices=last_size=0;frames=index+1;
        memset((void *)SS_IWRAM,0,0x8000);memset((void *)SS_EWRAM,0,0x40000);
        local_start();
        if(local_total!=expected) {fprintf(stderr,"catalog count %u != %u\n",local_total,expected);return 4;}
        for(i=0;i<local_total;i++) {
            rc_trigger_t *trigger=rc_runtime_get_achievement(&local_runtime,local_list[i].id);
            if(!trigger || trigger->state==RC_TRIGGER_STATE_DISABLED) return 5;
        }
        fread(&id,4,1,f);entry=local_find(id);
        rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
        if(entry->earned) {fprintf(stderr,"blank RAM unlock %u case %u\n",id,index);return 6;}
        apply(f);rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
        setup_unlocks += entry->earned != 0;
        apply(f);rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
        if(!entry->earned || !notices || !last_size) {fprintf(stderr,"not earned %u case %u badges %u\n",id,index,last_size);return 7;}
        passed++;rc_runtime_destroy(&local_runtime);free(local_list);
    }
    fclose(f);printf("{\"passed\":%u,\"cases\":%u,\"achievements\":%u,\"setup_unlocks\":%u}\n",passed,cases,expected,setup_unlocks);return 0;
}
"""
def main():
    catalog,rom,out=map(Path,sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
    body=json.loads(catalog.read_text());entries={a['ID']:a for s in body['Sets'] for a in s['Achievements'] if a['Flags']==3 and 0<a['ID']<101000001}
    scenarios=[]
    for entry in entries.values():
        for branch in entry['MemAddr'].split('S')[1:]:
            before,after=snapshot(branch);scenarios.append((entry['ID'],before,after))
    with (out/'cases.bin').open('wb') as f:
        f.write(struct.pack('<I',len(scenarios)))
        for ident,before,after in scenarios:
            f.write(struct.pack('<I',ident))
            for memory in (before,after):
                f.write(struct.pack('<I',len(memory)))
                for address,value in sorted(memory.items()): f.write(struct.pack('<IB',address,value))
    shutil.copy2(catalog,out/catalog.name)
    badges = out/'achievements'/'badges'
    badges.mkdir(parents=True, exist_ok=True)
    for source_dir in (catalog.parent/'badges', catalog.parent/'achievements'/'badges'):
        if source_dir.resolve() != badges.resolve():
            for image in source_dir.glob('badge_*.png'):
                shutil.copy2(image, badges/image.name)
    source=out/'verify.c';source.write_text(HARNESS)
    sdl=next(Path(h)/'x86_64-w64-mingw32' for h in link.SDL_HINTS if h and (Path(h)/'x86_64-w64-mingw32/lib').is_dir())
    objects=sorted((ROOT/'build/launcher').glob('rc_*.o'))
    cmd=[build.clang(),'--target=x86_64-w64-mingw32','-O1','-I',str(ROOT),'-I',str(ROOT/'runtime/include'),'-I',str(ROOT/'launcher/achievements'),'-I',str(ROOT/'third_party/rcheevos/include'),'-I',str(sdl/'include'),str(source),*[str(o) for o in objects],'-L',str(sdl/'lib'),'-lSDL3','-lwinhttp','-lcrypt32','-Wl,-Bstatic','-lwinpthread','-Wl,-Bdynamic','-o',str(out/'verify.exe')]
    subprocess.run(cmd,check=True);shutil.copy2(sdl/'bin/SDL3.dll',out/'SDL3.dll')
    result=subprocess.run([str(out/'verify.exe'),str(out),str(rom),str(out/'cases.bin'),str(len(entries))],capture_output=True,text=True)
    (out/'verification.log').write_text(result.stderr)
    print(result.stdout or result.stderr[-1200:]);result.check_returncode()
    report=json.loads(result.stdout);report.update(game_id=body['GameId'],points=sum(a['Points'] for a in entries.values()),method='synthetic RAM transitions through unchanged official conditions; not a playthrough; no online awards')
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
