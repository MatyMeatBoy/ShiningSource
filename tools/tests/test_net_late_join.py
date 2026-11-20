"""Exercise deferred room connections and existing-player recovery."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build


HARNESS = r"""
#include "runtime/core/netlink.c"
#include <assert.h>

static uint8_t incoming[HEADER + BLOCK + TRAILER + MSG_HEAD + SKIP_FOOTER];
static uint8_t hello[HEADER + 32];
static int incoming_size;
static int opens, closes, goodbyes;
static uint32_t now = 100;

void __real_sub_0800170C(void) {}
uint32_t __real_sub_08001804(void *sent, void *got) { return 0; }
unsigned long ss_frame_number(void) { return 0; }
uint32_t ss_plat_ticks_ms(void) { return now; }
uint32_t ss_rom_fingerprint(void) { return 0; }
void ss_prof(int stage, int begin) {}
int ss_net_code_message(void *message, int size, int receiving) { return 1; }
int ss_plat_net_open(const char *host, int port) {
    if (host) {
        assert(!strcmp(host, "127.0.0.1") && port == 47900);
        opens++;
    } else {
        closes++;
    }
    return 0;
}
int ss_plat_net_send(const void *bytes, int length) {
    const uint8_t *packet = bytes;
    if (packet[4] == T_HELLO) {
        assert(length == sizeof hello);
        memcpy(hello, bytes, length);
    } else if (packet[4] == T_END) {
        goodbyes++;
    }
    return length;
}
int ss_plat_net_recv(void *bytes, int capacity) {
    int length = incoming_size;
    assert(length <= capacity);
    memcpy(bytes, incoming, length);
    incoming_size = 0;
    return length;
}
void ss_plat_net_wait(int ms) {}
void ss_plat_net_farewell(const void *bytes, int length) {}
void ss_plat_status(const char *text) {}

static void queue_peer_data(int from, uint32_t from_epoch) {
    memset(incoming, 0, sizeof incoming);
    wr32(incoming, MAGIC);
    wr32(incoming + 8, room);
    wr32(incoming + 16, from_epoch);
    wr32(incoming + 20, FIRST_LABEL);
    wr32(incoming + 24, frame_label);
    incoming[4] = T_DATA;
    incoming[5] = from;
    incoming[7] = 1;
    incoming_size = HEADER + BLOCK;
}

static void peer_data(int from, uint32_t from_epoch) {
    queue_peer_data(from, from_epoch);
    receive();
}

static void welcome(int player, int seats) {
    memset(incoming, 0, sizeof incoming);
    wr32(incoming, MAGIC);
    wr32(incoming + 8, room);
    incoming[4] = T_WELCOME;
    incoming[5] = player;
    incoming[7] = seats;
    incoming_size = HEADER;
    ss_net_flush();
}

static void host_stage(enum ss_net_roster_stage stage, int start_block) {
    queue_peer_data(0, 15);
    if (start_block) {
        incoming[HEADER + 8] = 'G';
        incoming[HEADER + 9] = 'O';
    }
    incoming[6] = SKIP_CAPABILITY;
    incoming[HEADER + BLOCK] = TRAILER_MARK;
    incoming_size = sizeof incoming;
    incoming[incoming_size - SKIP_FOOTER + 14] = stage;
    ss_net_flush();
}

static void deferred_rooms(void) {
    char first[SS_ROOM_CODE_CAPACITY], second[SS_ROOM_CODE_CAPACITY];
    __wrap_sub_0800170C();
    ss_net_flush();
    assert(!opens && !hello[4] && ss_net_room_ui_enabled());
    assert(!ss_net_room_begin("BAD", 1));
    assert(!ss_net_room_begin("abcdI234", 1));
    ss_net_room_generate(first);
    ss_net_room_generate(second);
    assert(strlen(first) == SS_ROOM_CODE_LENGTH && strcmp(first, second));
    assert(strspn(first, SS_ROOM_CODE_ALPHABET) == SS_ROOM_CODE_LENGTH);
    assert(ss_net_room_begin("abcd2345", 1) && !opens);
    ss_net_flush();
    assert(opens == 1 && ss_net_room_status() == SS_ROOM_CONNECTING);
    assert(!strcmp((char *)hello + HEADER, "ABCD2345"));
    assert(rd32(hello + 12) == 1234);
    welcome(0, 1);
    assert(ss_net_room_status() == SS_ROOM_READY && seat == 0);
    ss_net_room_cancel();
    ss_net_flush();
    assert(closes == 1 && goodbyes == 2 && ss_net_room_status() == SS_ROOM_IDLE);

    assert(ss_net_room_begin("ABCD2345", 0));
    ss_net_flush();
    welcome(0, 1);
    assert(closes == 2 && goodbyes == 4 && ss_net_room_status() == SS_ROOM_UNAVAILABLE);
    assert(ss_net_room_begin("ABCD2345", 0));
    ss_net_flush();
    welcome(1, 3);
    assert(ss_net_room_status() == SS_ROOM_CONNECTING && seat == 1);
    ss_net_room_cancel();
    ss_net_flush();
    assert(closes == 3 && goodbyes == 6);

    assert(ss_net_room_begin("ABCD2345", 0));
    ss_net_flush();
    welcome(1, 3);
    host_stage(SS_ROSTER_IN_GAME, 0);
    assert(closes == 4 && goodbyes == 8 && ss_net_room_status() == SS_ROOM_ACTIVE);
    assert(ss_net_room_begin("ABCD2345", 0));
    ss_net_flush();
    welcome(1, 3);
    host_stage(SS_ROSTER_READY, 1);
    assert(closes == 5 && goodbyes == 10 && ss_net_room_status() == SS_ROOM_ACTIVE);
    assert(ss_net_room_begin("ABCD2345", 0));
    ss_net_flush();
    welcome(1, 3);
    host_stage(SS_ROSTER_CREATING_CHARACTER, 0);
    assert(ss_net_room_status() == SS_ROOM_READY && seat == 1);
    ss_net_room_cancel();
    ss_net_flush();
    assert(closes == 6 && goodbyes == 12);

    assert(ss_net_room_begin("ABCD2345", 1));
    ss_net_flush();
    now += 5000;
    ss_net_flush();
    assert(closes == 7 && goodbyes == 12 && ss_net_room_status() == SS_ROOM_UNAVAILABLE);
}

int main(void) {
    if (ss_net_room_ui_enabled()) {
        deferred_rooms();
        return 0;
    }
    if (getenv("SS_NET_DEFAULT")) {
        __wrap_sub_0800170C();
        assert(!opens);
    } else if (getenv("SS_NET")) {
        __wrap_sub_0800170C();
        assert(opens == 1 && !strcmp((char *)hello + HEADER, "LegacyParty"));
        return 0;
    }
    const int guest = 3;
    const unsigned guest_bit = 1u << guest;
    uint8_t send_block[BLOCK] = {0}, receive_blocks[SEATS * BLOCK];
    configured = 1;
    seat = 0;
    direct = 1;
    start_session();
    connected = 7;
    peer_data(guest, 21);
    assert(streams[guest].known);                  /* An open lobby accepts the last seat. */
    memset(&streams[guest], 0, sizeof streams[guest]);

    send_block[8] = 'G';
    send_block[9] = 'O';
    deliver_direct(send_block, receive_blocks);
    assert(lobby_done);
    peer_data(guest, 21);
    assert(!streams[guest].known);                 /* GO closes the lobby immediately. */
    lobby_done = 0;
    in_play = 1;
    peer_data(guest, 21);
    assert(!streams[guest].known);                 /* An unknown seat cannot enter mid-game. */

    connected |= guest_bit;
    gone |= guest_bit;
    away[guest].epoch = 21;
    away[guest].msg_acked = out_next;
    peer_data(guest, 22);
    assert((gone & guest_bit) && !streams[guest].known);
    peer_data(guest, 21);
    assert(!(gone & guest_bit) && streams[guest].known);  /* The same running player returns. */
    return 0;
}
"""


class NetLateJoinTests(unittest.TestCase):
    def test_live_room_rejects_new_streams_and_accepts_a_known_return(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "late_join.c"
            output = Path(directory) / "late_join.exe"
            source.write_text(HARNESS, encoding="ascii")
            subprocess.run([build.clang(), "-I", str(ROOT), "-I", str(ROOT / "runtime/include"),
                            str(source), "-o", str(output)], check=True)
            env = {key: value for key, value in os.environ.items() if not key.startswith("SS_NET")}
            subprocess.run([str(output)], cwd=directory, env=env, timeout=5, check=True)
            env.update(SS_NET="127.0.0.1:47900", SS_NET_DEFERRED="1", SS_NET_TOKEN="1234")
            subprocess.run([str(output)], cwd=directory, env=env, timeout=5, check=True)
            env.pop("SS_NET")
            env.pop("SS_NET_DEFERRED")
            env.pop("SS_FRAMES", None)
            env["SS_NET_DEFAULT"] = "127.0.0.1:47900"
            subprocess.run([str(output)], cwd=directory, env=env, timeout=5, check=True)
            env["SS_FRAMES"] = "10"
            subprocess.run([str(output)], cwd=directory, env=env, timeout=5, check=True)
            env.pop("SS_NET_DEFAULT")
            env.update(SS_NET="127.0.0.1:47900", SS_NET_ROOM="LegacyParty")
            subprocess.run([str(output)], cwd=directory, env=env, timeout=5, check=True)


if __name__ == "__main__":
    unittest.main()
