"""Exercise the game's actual RA evaluator, local persistence and badge notices."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build
import link
ROOT = Path(__file__).resolve().parents[2]
HARNESS = r"""
#include "launcher/achievements/ra_client.c"
#include "runtime/achievements/ach_host.c"
static int notices, badge_size;
void ss_plat_notice(const char *heading, const char *text, int rebel, const uint32_t *icon, int size) {
    notices++; badge_size = size;
}
int main(int argc, char **argv) {
    uint8_t rom[1024] = {0}; char hash[33], name[1200];
    rc_hash_iterator_t it; SDL_Surface *surface;
    if (argc != 2) return 1;
    snprintf(folder_utf8, sizeof folder_utf8, "%s", argv[1]);
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, folder, 900);
    if (VirtualAlloc((void *)SS_IWRAM, 0x8000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE) != (void *)SS_IWRAM) return 2;
    rom_bytes=rom; rom_size=sizeof rom;
    local_start();
    if (local_total != 0) return 3; /* no invented achievements without a catalog */
    rc_hash_initialize_iterator(&it,NULL,rom,sizeof rom);
    rc_hash_generate(hash,RC_CONSOLE_GAMEBOY_ADVANCE,&it); rc_hash_destroy_iterator(&it);
    snprintf(name,sizeof name,"%s/ra_set_%s.json",argv[1],hash);
    { char source[1200]; snprintf(source,sizeof source,"%s/set.json",argv[1]); CopyFileA(source,name,FALSE); }
    surface=SDL_CreateSurface(8,8,SDL_PIXELFORMAT_ARGB8888);
    snprintf(name,sizeof name,"%s/badge_12345.png",argv[1]); SDL_SavePNG(surface,name); SDL_DestroySurface(surface);
    rom[1] = 42; /* native pointer patching must not change the catalog hash */
    active=1; frames=59; ss_ach_frame(0);
    if (local_total != 1 || local_list[0].id != 123) return 4;
    rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
    *(uint8_t *)(SS_IWRAM+0x10)=1;
    rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
    if (local_earned != 1 || notices != 1 || badge_size != 8) return 5;
    rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
    if (notices != 1) return 6;
    { /* Re-earning on an account must still notify after an earlier local unlock. */
        rc_client_achievement_t info = {0}; rc_client_event_t event = {0};
        ra=KhRaCreate(folder_utf8,read_memory,NULL,NULL);
        info.id=123; info.title="Official fixture"; info.description="Memory condition";
        snprintf(info.badge_name,sizeof info.badge_name,"12345");
        event.type=RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED; event.achievement=&info;
        frames++; Event(&event,KhRaClient(ra)); account_notices();
        if (notices != 2) return 10;
        Event(&event,KhRaClient(ra)); info.id=124; Event(&event,KhRaClient(ra));
        if (!KhRaTakeNotice(ra) || KhRaNoticeId(ra)!=123) return 11;
        if (!KhRaTakeNotice(ra) || KhRaNoticeId(ra)!=124 || KhRaTakeNotice(ra)) return 12;
        KhRaDestroy(ra); ra=NULL; notices=1;
    }
    rc_runtime_destroy(&local_runtime); free(local_list); local_list=NULL; local_total=local_earned=0;
    local_start();
    if (local_total != 1 || local_earned != 1) return 7;
    rc_runtime_destroy(&local_runtime); free(local_list); local_list=NULL; local_total=local_earned=0;
    rebel_run=1; local_start();
    if (local_earned != 0) return 8; /* normal progress never becomes Rebel progress */
    *(uint8_t *)(SS_IWRAM+0x10)=0;
    rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
    *(uint8_t *)(SS_IWRAM+0x10)=1;
    rc_runtime_do_frame(&local_runtime,local_event,local_peek,NULL,NULL);
    if (local_earned != 1 || notices != 2 || badge_size != 8) return 9;
    return 0;
}
"""
class AchievementsTest(unittest.TestCase):
    def test_official_conditions_persist_and_show_badge(self):
        sdl = next(Path(h)/'x86_64-w64-mingw32' for h in link.SDL_HINTS if h and (Path(h)/'x86_64-w64-mingw32/lib').is_dir())
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp)
            entry=dict(ID=123,Title='Official fixture',Description='Memory condition',Flags=3,Points=5,MemAddr='0xH000010=1',Author='fixture',BadgeName='12345',Created=0,Modified=0)
            unofficial=dict(entry,ID=124,Flags=5)
            warning=dict(entry,ID=101000001,Title='Warning: Unknown Emulator',Points=0,MemAddr='1=1.300.')
            catalog=dict(Success=True,GameId=1,Title='Fixture',ConsoleId=5,ImageIconUrl='/Images/1.png',Sets=[dict(AchievementSetId=1,GameId=1,Title='Fixture',Type='core',ImageIconUrl='/Images/1.png',Achievements=[entry,unofficial,warning],Leaderboards=[])])
            (directory/'set.json').write_text(json.dumps(catalog))
            # A retired custom unlock must not affect official progress.
            (directory/'local_record.txt').write_text('2130706432\t1\tFirst Steps\n')
            source=directory/'test.c'; source.write_text(HARNESS)
            objects=[o for o in (ROOT/'build/launcher').glob('rc_*.o') if o.name != 'rc_achievements_ra_client.o']
            self.assertTrue(objects,'Build the launcher first')
            command=[build.clang(),'--target=x86_64-w64-mingw32','-O0','-DRC_CLIENT_SUPPORTS_HASH','-I',str(ROOT),'-I',str(ROOT/'runtime/include'),'-I',str(ROOT/'launcher/achievements'),'-I',str(ROOT/'third_party/rcheevos/include'),'-I',str(sdl/'include'),str(source),*[str(o) for o in objects],'-L',str(sdl/'lib'),'-lSDL3','-lwinhttp','-lcrypt32','-Wl,-Bstatic','-lwinpthread','-Wl,-Bdynamic','-o',str(directory/'test.exe')]
            compiled=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            import shutil
            shutil.copy2(sdl/'bin/SDL3.dll',directory/'SDL3.dll')
            result=subprocess.run([str(directory/'test.exe'),str(directory)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            lines=(directory/'local_record.txt').read_text().splitlines()
            self.assertEqual(sum(line.startswith('123\t') for line in lines),1)
            self.assertEqual(len((directory/'rebel_record.txt').read_text().splitlines()),1)
if __name__=='__main__': unittest.main()
