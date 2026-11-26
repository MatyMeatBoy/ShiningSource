"""The account's queue of unlocks earned without a connection (launcher/achievements/ra_client.c).

A handheld queues what the local evaluation earned (KhRaQueueLocal) and sends it at the next start online. Its clock
may say 1970: such an unlock must go out as just earned (no seconds_since_unlock), a believable one with its age.
The site is a C function here: nothing leaves the machine.
"""
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build
import link

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r"""
#include "launcher/achievements/ra_client.c"
static char posts[16][2048];
static int post_count;
static const char *answer(const char *post) {
    if (strstr(post, "r=login2")) return "{\"Success\":true,\"User\":\"tester\",\"Token\":\"fixture\",\"Score\":0,\"SoftcoreScore\":0,\"Messages\":0}";
    if (strstr(post, "r=achievementsets")) return "{\"Success\":true,\"GameId\":7095,\"Title\":\"T\",\"ConsoleId\":5,\"ImageIconUrl\":\"/Images/1.png\",\"Sets\":[{\"AchievementSetId\":1,\"GameId\":7095,\"Title\":null,\"Type\":\"core\",\"ImageIconUrl\":\"/Images/1.png\",\"Achievements\":[{\"ID\":900001,\"Title\":\"A\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH000010=1\",\"Author\":\"f\",\"BadgeName\":\"1\",\"Created\":0,\"Modified\":0},{\"ID\":900002,\"Title\":\"B\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH000011=1\",\"Author\":\"f\",\"BadgeName\":\"1\",\"Created\":0,\"Modified\":0},{\"ID\":900003,\"Title\":\"C\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH000012=1\",\"Author\":\"f\",\"BadgeName\":\"1\",\"Created\":0,\"Modified\":0}],\"Leaderboards\":[]}]}";
    if (strstr(post, "r=startsession")) return "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1767225600}";
    if (strstr(post, "r=unlocks")) return "{\"Success\":true,\"UserUnlocks\":[900003],\"GameID\":7095,\"HardcoreMode\":false}";
    if (strstr(post, "r=awardachievement")) return "{\"Success\":true,\"Score\":1,\"SoftcoreScore\":1,\"AchievementID\":900001,\"AchievementsRemaining\":0}";
    return "{\"Success\":true}";
}
static void transport(const rc_api_request_t *request, rc_client_server_callback_t callback, void *data) {
    rc_api_server_response_t response = { "", 0, 200 };
    const char *post = request->post_data ? request->post_data : "";
    if (post_count < 16) snprintf(posts[post_count++], sizeof posts[0], "%s", post);
    response.body = answer(post);
    response.body_length = strlen(response.body);
    if (!request->post_data) response.http_status_code = 404;
    callback(&response, data);
}
static uint32_t no_memory(uint32_t a, uint8_t *o, uint32_t s, void *u) { (void)a; (void)o; (void)s; (void)u; return 0; }
int main(int argc, char **argv) {
    uint8_t rom[1024] = {0};
    long long now = (long long)time(NULL);
    int i;
    KhRa *ra = KhRaCreate(argv[1], no_memory, NULL, transport);
    if (!ra) return 2;
    if (KhRaQueueLocal(ra, 900001, now)) return 3;          /* no account yet: nothing is queued */
    KhRaSetEnabled(ra, 1);
    snprintf(ra->username, sizeof ra->username, "tester"); snprintf(ra->token, sizeof ra->token, "fixture");
    if (!KhRaQueueLocal(ra, 900001, 7) || !KhRaQueueLocal(ra, 900002, now - 3600) || !KhRaQueueLocal(ra, 900003, now) ||
        !KhRaQueueLocal(ra, 900001, now) || KhRaQueueLocal(ra, 101000001u, now)) return 4;
    if (KhRaPendingCount(ra) != 3) return 5;              /* one entry per achievement; never the warning id */
    ra->username[0] = ra->token[0] = 0;
    KhRaLogin(ra, "tester", "not-a-password", 0);
    KhRaStartOfficial(ra, NULL, 0);
    KhRaSetRom(ra, rom, sizeof rom);
    for (i = 0; i < 4; i++) KhRaPump(ra, 0);
    for (i = 0; i < post_count; i++) {
        char *token = strstr(posts[i], "t=");
        if (token) { token += 2; while (*token && *token != '&') *token++ = 'x'; }  /* never print a token */
        printf("%s\n", posts[i]);
    }
    printf("pending %u\n", KhRaPendingCount(ra));
    KhRaDestroy(ra);
    return 0;
}
"""


class PendingQueueTest(unittest.TestCase):
    def test_queue_is_sent_with_believable_ages_only(self):
        sdl = next(Path(h) / 'x86_64-w64-mingw32' for h in link.SDL_HINTS
                   if h and (Path(h) / 'x86_64-w64-mingw32/lib').is_dir())
        objects = [o for o in (ROOT / 'build/launcher').glob('rc_*.o') if o.name != 'rc_achievements_ra_client.o']
        self.assertTrue(objects, 'Build the launcher first')
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / 'test.c').write_text(HARNESS)
            command = [build.clang(), '--target=x86_64-w64-mingw32', '-O0', '-DRC_CLIENT_SUPPORTS_HASH', '-I', str(ROOT),
                       '-I', str(ROOT / 'launcher/achievements'), '-I', str(ROOT / 'third_party/rcheevos/include'),
                       '-I', str(ROOT / 'third_party/rcheevos/src'), '-I', str(sdl / 'include'), str(directory / 'test.c'),
                       *map(str, objects), '-L', str(sdl / 'lib'), '-lSDL3', '-lwinhttp', '-lcrypt32',
                       '-Wl,-Bstatic', '-lwinpthread', '-Wl,-Bdynamic', '-o', str(directory / 'test.exe')]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            import shutil
            shutil.copy2(sdl / 'bin/SDL3.dll', directory / 'SDL3.dll')
            folder = directory / 'ra'
            folder.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith('SS_')}
            env['APPDATA'] = str(directory)
            result = subprocess.run([str(directory / 'test.exe'), str(folder)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            lines = result.stdout.splitlines()
            awards = {}
            for line in lines:
                fields = dict(f.split('=', 1) for f in line.split('&') if '=' in f)
                if fields.get('r') == 'awardachievement':
                    awards[fields['a']] = fields
            # the account already lists 900003: dropped, not sent
            self.assertEqual(sorted(awards), ['900001', '900002'], lines)
            self.assertNotIn('o', awards['900001'])          # earned "in 1970": sent as just earned
            self.assertTrue(3500 <= int(awards['900002']['o']) <= 3700, awards['900002'])
            self.assertTrue(all(f.get('h') == '0' for f in awards.values()))   # softcore only
            self.assertIn('pending 0', lines)


if __name__ == '__main__':
    unittest.main()
