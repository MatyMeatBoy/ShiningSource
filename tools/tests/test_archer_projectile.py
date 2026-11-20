"""Exercise the arrow's pending-launch wait and shared slot cleanup."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build, prepared_source


HARNESS = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t s32;
typedef uint32_t u32;
struct Ply5778 { unsigned int ready, state; };
static struct Ply5778 actor;
static int yields, scene_change, event_lock;
static const char *scenario;
static u32 m5778[4];

static int sub_08056E90(void) { return scene_change; }
static int sub_0800D11C(void) { return event_lock; }
static void sub_080003F0(void) {
    // The actor gets one frame before the projectile task resumes.
    if (++yields > 1) exit(4);
    if (!strcmp(scenario, "normal")) actor.ready = 1;
    if (!strcmp(scenario, "hurt")) actor.state = 10;
    if (!strcmp(scenario, "dead")) actor.state = 11;
    if (!strcmp(scenario, "inactive")) actor.state = 12;
    if (!strcmp(scenario, "area")) scene_change = 1;
    if (!strcmp(scenario, "event")) event_lock = 1;
}

@WAIT@

static int arrow(u32 who, u32 slot) {
    int launched = 0;
@LAUNCH@
    launched = 1;
@RELEASE@
    return launched;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    scenario = argv[1];
    int charge = !strcmp(scenario, "charged");
    int fires = charge || !strcmp(scenario, "normal");
    actor.state = charge ? 5 : 2;
    actor.ready = charge ? 3 : 0;
    // All slots are occupied. A cancelled arrow must release its own slot,
    // leaving the other tasks and the other actor's pool intact.
    m5778[2] = m5778[3] = 0xFFFF;
    if (arrow(2, 5) != fires) return 2;
    if (m5778[2] != (0xFFFF & ~(1u << 5)) || m5778[3] != 0xFFFF) return 3;
    if (yields != !charge) return 5;
    return 0;
}
'''


class ArcherProjectile(unittest.TestCase):
    def test_shot_and_charge_launch_and_cancelled_arrows_release_slots(self):
        source = prepared_source('code_08073E2C.c')
        start = source.index('static inline s32 wait5778(')
        wait = source[start:source.index('\n}', start) + 2]
        start = source.index('    if (wait5778(p, 1))', start)
        launch = '    struct Ply5778 *p = &actor;\n' + source[start:source.index('goto end;', start) + len('goto end;')]
        start = source.index('\nend:', start)
        release = source[start:source.index('\n}', start)]
        fixture = HARNESS.replace('@WAIT@', wait).replace('@LAUNCH@', launch).replace('@RELEASE@', release)

        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            c_file, executable = out / 'projectile.c', out / 'projectile.exe'
            c_file.write_text(fixture, encoding='utf-8')
            subprocess.run([build.clang(), '-std=c11', str(c_file), '-o', str(executable)],
                           check=True, cwd=ROOT)
            for scenario in ('normal', 'charged', 'hurt', 'dead', 'inactive', 'area', 'event'):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(executable), scenario], check=False, timeout=5)
                    self.assertEqual(result.returncode, 0, '4 means the slot is still occupied after cancellation')


if __name__ == '__main__':
    unittest.main()
