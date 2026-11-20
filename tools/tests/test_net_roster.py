"""Exercise GameLink status on the real packet path, with no ROM or relay."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build


class NetRoster(unittest.TestCase):
    def test_roster_follows_live_seats_and_ignores_stale_packets(self):
        harness = r'''
#include "runtime/core/netlink.c"

static uint8_t incoming[PACKET_MAX], outgoing[PACKET_MAX];
static int incoming_size, outgoing_size;
static uint32_t now = 100;

void __real_sub_0800170C(void) {}
uint32_t __real_sub_08001804(void *send, void *recv) { return 0; }
unsigned long ss_frame_number(void) { return 0; }
uint32_t ss_plat_ticks_ms(void) { return now; }
uint32_t ss_rom_fingerprint(void) { return 0; }
void ss_prof(int stage, int begin) {}
int ss_net_code_message(void *data, int length, int receiving) { return 1; }
int ss_plat_net_open(const char *host, int port) { return 0; }
int ss_plat_net_send(const void *data, int length)
{
    memcpy(outgoing, data, length);
    outgoing_size = length;
    return length;
}
int ss_plat_net_recv(void *data, int capacity)
{
    int size = incoming_size;
    memcpy(data, incoming, size);
    incoming_size = 0;
    return size;
}
void ss_plat_net_wait(int ms) {}
void ss_plat_net_farewell(const void *data, int length) {}
void ss_plat_status(const char *text) {}

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static void from_peer(int from, uint32_t stream_epoch, uint32_t label, int has_footer,
                      int stage, int class_id, int palette_id)
{
    uint8_t *trailer = incoming + HEADER + BLOCK;
    uint8_t *message = trailer + TRAILER;
    uint8_t *footer = message + MSG_HEAD;
    memset(incoming, 0, sizeof incoming);
    wr32(incoming, MAGIC);
    wr32(incoming + 8, room);
    wr32(incoming + 16, stream_epoch);
    wr32(incoming + 20, FIRST_LABEL);
    wr32(incoming + 24, label);
    incoming[4] = T_DATA;
    incoming[5] = from;
    incoming[6] = has_footer ? SKIP_CAPABILITY : 0;
    incoming[7] = 1;
    trailer[0] = TRAILER_MARK;
    message[0] = MSG_MARK;
    footer[14] = (uint8_t)(stage | ((palette_id + 1) << 3));
    footer[15] = (uint8_t)(class_id + 1);
    incoming_size = (int)(footer - incoming) + (has_footer ? SKIP_FOOTER : 0);
    receive();
}

int main(void)
{
    struct ss_net_roster_entry rows[4];
    configured = 1;
    seat = 0;
    start_session();
    CHECK(!ss_net_roster(rows) && !ss_net_roster_lobby());
    ss_net_roster_set_stage(SS_ROSTER_SELECTING_SAVE);
    CHECK(ss_net_roster(rows) == 1 && rows[0].local);
    CHECK(rows[0].stage == SS_ROSTER_SELECTING_SAVE && !ss_net_roster_lobby());
    from_peer(1, 81, FIRST_LABEL, 0, 0, -1, -1);
    from_peer(3, 83, FIRST_LABEL, 1, 0, -1, -1);
    CHECK(ss_net_roster(rows) == 3);
    CHECK(rows[1].stage == SS_ROSTER_CONNECTED && rows[1].class_id == -1);
    CHECK(rows[2].stage == SS_ROSTER_EMPTY && rows[3].stage == SS_ROSTER_CONNECTED);
    from_peer(1, 81, FIRST_LABEL + 1, 1, SS_ROSTER_CREATING_CHARACTER, 4, 2);
    CHECK(ss_net_roster(rows) == 3 && rows[1].stage == SS_ROSTER_CREATING_CHARACTER);
    CHECK(rows[1].class_id == -1);
    from_peer(1, 81, FIRST_LABEL + 2, 1, SS_ROSTER_READY, 2, 3);
    from_peer(1, 81, FIRST_LABEL + 1, 1, SS_ROSTER_CREATING_CHARACTER, -1, -1);
    CHECK(ss_net_roster(rows) == 3 && rows[1].stage == SS_ROSTER_READY);
    CHECK(rows[1].class_id == 2 && rows[1].palette_id == 3);
    from_peer(1, 81, FIRST_LABEL + 3, 1, SS_ROSTER_SELECTING_SAVE, -1, -1);
    CHECK(ss_net_roster(rows) == 3 && rows[1].stage == SS_ROSTER_SELECTING_SAVE);
    CHECK(rows[1].class_id == -1 && rows[1].palette_id == -1);
    from_peer(1, 81, FIRST_LABEL + 4, 1, SS_ROSTER_IN_GAME, 2, 3);
    CHECK(ss_net_roster(rows) == 3 && rows[1].stage == SS_ROSTER_IN_GAME);
    now += 2001;
    CHECK(ss_net_roster(rows) == 3 && rows[1].stage == SS_ROSTER_RECONNECTING);
    from_peer(1, 91, FIRST_LABEL, 0, 0, -1, -1);
    CHECK(ss_net_roster(rows) == 3 && rows[1].stage == SS_ROSTER_CONNECTED);
    CHECK(rows[1].class_id == -1 && rows[1].palette_id == -1);

    ss_net_roster_set_character(8, 2);
    ss_net_roster_set_stage(SS_ROSTER_READY);
    CHECK(ss_net_roster(rows) == 3 && ss_net_roster_lobby() && !ss_net_roster_can_start());
    ss_net_roster_set_host_start(1);
    CHECK(ss_net_roster_can_start());
    CHECK(rows[0].class_id == 8 && rows[0].palette_id == 2);
    send_data();
    CHECK(outgoing_size >= SKIP_FOOTER);
    CHECK(outgoing[outgoing_size - 2] == (SS_ROSTER_READY | 3 << 3));
    CHECK(outgoing[outgoing_size - 1] == 9);
    offline_since = now;
    CHECK(ss_net_roster(rows) == 3 && rows[0].stage == SS_ROSTER_RECONNECTING && !ss_net_roster_can_start());
    offline_since = 0;
    ss_net_roster_set_stage(SS_ROSTER_IN_GAME);
    CHECK(!ss_net_roster(rows) && !ss_net_roster_lobby() && !ss_net_roster_can_start());
    send_data();
    CHECK(outgoing[outgoing_size - 2] == (SS_ROSTER_IN_GAME | 3 << 3));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'net_roster.c'
            exe = Path(directory) / 'net_roster.exe'
            source.write_text(harness, encoding='ascii')
            result = subprocess.run([build.clang(), '-I', str(ROOT), '-I', str(ROOT / 'runtime/include'),
                                     str(source), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, f'roster check failed at line {result.returncode}\n{result.stderr}')
