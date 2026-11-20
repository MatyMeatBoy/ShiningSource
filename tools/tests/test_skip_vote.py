"""Exercise the host's actual scene-skip ballot rules without a network or ROM."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build


class SkipVote(unittest.TestCase):
    def compile_and_run(self, harness):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'test.c'
            output = Path(directory)/'test.exe'
            source.write_text(harness, encoding='ascii')
            compile_result = subprocess.run([build.clang(), '-I', str(ROOT), '-I', str(ROOT/'runtime/include'),
                                             str(source), '-o', str(output)], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(output)], cwd=directory, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, f'ballot check failed at fixture line {result.returncode}\n{result.stderr}')

    def test_each_player_gets_one_initiation_per_scene(self):
        self.compile_and_run(r'''
#include "runtime/core/skip_vote.h"
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

int main(void)
{
    struct skip_ballot vote = {0};
    unsigned scene;
    skip_ballot_update(&vote, 4, 1, 7, 100);
    scene = vote.scene;
    skip_ballot_press(&vote, scene, 0, 1, SKIP_START, 100);
    CHECK(vote.owner == 2 && vote.votes == 2 && vote.spent == 2);
    skip_ballot_press(&vote, scene, 0, 2, SKIP_START, 110);
    CHECK(vote.votes == 2);      /* a stale initiation cannot become a yes */
    skip_ballot_press(&vote, scene, 2, 0, SKIP_START, 120);
    CHECK(!skip_ballot_ready(&vote));
    skip_ballot_press(&vote, scene, 2, 0, SKIP_BACK, 130);
    CHECK(vote.votes == 2);
    skip_ballot_press(&vote, scene, 2, 1, SKIP_START, 140);
    CHECK(!vote.owner && !vote.votes);
    skip_ballot_press(&vote, scene, 0, 1, SKIP_START, 150);
    CHECK(!vote.owner && vote.spent == 2);

    skip_ballot_press(&vote, scene, 0, 2, SKIP_START, 200);
    skip_ballot_press(&vote, scene, 3, 1, SKIP_START, 220);
    CHECK(vote.votes == 6);      /* a spent initiator can vote in another ballot */
    skip_ballot_update(&vote, 4, 1, 7, 30199);
    CHECK(vote.owner == 3);
    skip_ballot_update(&vote, 4, 1, 7, 30200);
    CHECK(!vote.owner && vote.spent == 6);
    skip_ballot_press(&vote, scene, 3, 0, SKIP_START, 30210);
    CHECK(!vote.owner);          /* the old ballot expired before this arrived */
    skip_ballot_press(&vote, scene, 0, 2, SKIP_START, 30220);
    CHECK(!vote.owner);
    skip_ballot_press(&vote, scene, 0, 0, SKIP_START, 30230);
    skip_ballot_press(&vote, scene, 1, 1, SKIP_START, 30240);
    CHECK(!skip_ballot_ready(&vote));
    skip_ballot_press(&vote, scene, 1, 2, SKIP_START, 30250);
    CHECK(skip_ballot_ready(&vote));

    skip_ballot_update(&vote, 4, 0, 7, 30300);   /* dialogue question, still the same scene */
    CHECK(!vote.owner && vote.spent == 7 && vote.scene == scene);
    skip_ballot_update(&vote, 0, 0, 7, 30400);
    skip_ballot_update(&vote, 4, 1, 7, 30500);
    CHECK(vote.scene != scene && !vote.spent);
    skip_ballot_press(&vote, scene, 0, 1, SKIP_START, 30510);
    CHECK(!vote.owner);
    skip_ballot_press(&vote, vote.scene, 0, 1, SKIP_START, 30520);
    CHECK(vote.owner == 2);
    skip_ballot_update(&vote, 4, 1, 5, 30530);   /* owner left */
    CHECK(!vote.owner && vote.spent == 2);
    skip_ballot_update(&vote, 4, 1, 7, 30540);
    skip_ballot_press(&vote, vote.scene, 0, 0, SKIP_START, 30550);
    skip_ballot_press(&vote, vote.scene, 1, 1, SKIP_START, 30560);
    CHECK(!skip_ballot_ready(&vote));
    skip_ballot_update(&vote, 4, 1, 3, 30570);   /* the abstaining player left */
    CHECK(!vote.owner && !skip_ballot_ready(&vote) && vote.spent == 3);

    skip_ballot_update(&vote, 0, 0, 15, 31000);
    skip_ballot_update(&vote, 4, 1, 15, 31010);
    for (unsigned player = 0; player < 4; ++player) {
        skip_ballot_press(&vote, vote.scene, 0, player, SKIP_START, 31100 + player);
        CHECK(vote.owner == player + 1);
        skip_ballot_press(&vote, vote.scene, vote.owner, player, SKIP_BACK, 31110 + player);
        CHECK(!vote.owner && !vote.votes);
    }
    CHECK(vote.spent == 15);
    for (unsigned player = 0; player < 4; ++player) {
        skip_ballot_press(&vote, vote.scene, 0, player, SKIP_START, 31120 + player);
        CHECK(!vote.owner);
    }
    return 0;
}
''')

    def test_real_netlink_rejects_stale_actions_and_keeps_legacy_rooms_legacy(self):
        self.compile_and_run(r'''
#include "runtime/core/netlink.c"

static uint8_t incoming[PACKET_MAX], outgoing[PACKET_MAX];
static int incoming_length, outgoing_length;
static uint32_t now = 100;

void __real_sub_0800170C(void) { }
uint32_t __real_sub_08001804(void *sent, void *got) { (void)sent; (void)got; return 0; }
unsigned long ss_frame_number(void) { return 0; }
uint32_t ss_plat_ticks_ms(void) { return now; }
uint32_t ss_rom_fingerprint(void) { return 0; }
void ss_prof(int stage, int begin) { (void)stage; (void)begin; }
int ss_net_code_message(void *message, int size, int receiving) { (void)message; (void)size; (void)receiving; return 1; }
int ss_plat_net_open(const char *host, int port) { (void)host; (void)port; return 0; }
int ss_plat_net_send(const void *bytes, int length)
{
    if (length > PACKET_MAX) exit(250);
    outgoing_length = length;
    memcpy(outgoing, bytes, length);
    return length;
}
int ss_plat_net_recv(void *bytes, int capacity)
{
    int type = incoming_length;
    if (type > capacity) exit(251);
    memcpy(bytes, incoming, type);
    incoming_length = 0;
    return type;
}
void ss_plat_net_wait(int ms) { (void)ms; }
void ss_plat_net_farewell(const void *bytes, int length) { (void)bytes; (void)length; }
void ss_plat_status(const char *text) { (void)text; }

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static void peer_data(int from, int modern, unsigned action, unsigned scene, unsigned owner, unsigned button)
{
    uint8_t *trailer = incoming + HEADER + BLOCK;
    uint8_t *messages = trailer + TRAILER;
    uint8_t *footer = messages + MSG_HEAD;
    memset(incoming, 0, sizeof incoming);
    wr32(incoming, MAGIC);
    wr32(incoming + 8, room);
    wr32(incoming + 16, streams[from].epoch);
    wr32(incoming + 20, FIRST_LABEL);
    wr32(incoming + 24, frame_label);
    incoming[4] = T_DATA;
    incoming[5] = from;
    incoming[6] = 4 | (modern ? SKIP_CAPABILITY : 0);
    incoming[7] = 1;
    trailer[0] = TRAILER_MARK;
    messages[0] = MSG_MARK;
    wr32(footer, action);
    wr32(footer + 4, scene);
    footer[9] = owner;
    footer[13] = button;
    incoming_length = (int)(footer - incoming) + (modern ? SKIP_FOOTER : 0);
    receive();
}

int main(void)
{
    struct ss_skip_ballot_view result;
    const uint8_t *footer;
    unsigned scene;
    configured = 1;
    seat = 0;
    start_session();
    for (unsigned i = 1; i <= 2; ++i) {
        streams[i].known = 1;
        streams[i].epoch = 3 + i;
        streams[i].next = FIRST_LABEL;
    }
    CHECK(ss_net_skip_ballot(4, 1, 0, &result) && result.need == 3);
    scene = ballot.scene;
    peer_data(1, 1, 1, scene, 0, SKIP_START);
    peer_data(2, 1, 1, scene, 0, SKIP_START);
    CHECK(ss_net_skip_ballot(4, 1, 0, &result) && result.have == 1 && ballot.owner == 2);
    peer_data(1, 1, 2, scene, 2, SKIP_START);
    peer_data(1, 1, 1, scene, 0, SKIP_START); /* reordered old initiation */
    CHECK(ss_net_skip_ballot(4, 1, 0, &result) && !ballot.owner && ballot.spent == 2);
    peer_data(2, 1, 2, scene, 2, SKIP_START); /* old yes is not the next start */
    CHECK(ss_net_skip_ballot(4, 1, 0, &result) && !ballot.owner);

    CHECK(ss_net_skip_ballot(4, 1, SKIP_START, &result) && result.owns);
    peer_data(1, 1, 3, scene, 1, SKIP_START);
    peer_data(2, 1, 3, scene, 1, SKIP_START);
    CHECK(ss_net_skip_ballot(4, 1, 0, &result) && result.have == 3);
    CHECK(!ss_net_skip_now() && skip_at > frame_label);
    send_data();
    footer = outgoing + outgoing_length - SKIP_FOOTER;
    CHECK((outgoing[6] & SKIP_CAPABILITY) && (outgoing[6] & 0x3F) == delay);
    CHECK(outgoing[HEADER + BLOCK + 1] == 0);
    CHECK(outgoing[HEADER + BLOCK + TRAILER] == MSG_MARK && footer[13] == 1 && footer[10] == 7);

    /* A legacy peer remains a legacy participant after an outage, so its return never inherits
       a strict skip the rest accepted while it was away. The fallback still requires legacy yeses. */
    start_session();
    for (unsigned i = 1; i <= 3; ++i) {
        streams[i].known = 1;
        streams[i].epoch = 3 + i;
    }
    peer_data(3, 0, 0, 0, 0, 0);
    CHECK(!ss_net_skip_ballot(4, 1, 0, &result));
    streams[3].known = 0;
    CHECK(!ss_net_skip_ballot(4, 1, 0, &result));
    CHECK(!ss_net_skip_now() && !skip_at);
    ss_net_vote(1);
    streams[1].vote = streams[2].vote = 1;
    CHECK(!ss_net_skip_now() && skip_at > frame_label);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
