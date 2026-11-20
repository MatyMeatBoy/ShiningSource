"""Exercise host field-item pickup replies for normal and stale requests."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import ROOT, build, prepared_source


class FieldItemPickup(unittest.TestCase):
    def test_host_replies_when_pickup_record_has_already_been_claimed_or_removed(self):
        receiver = prepared_source('code_080659B0.c')
        start = receiver.index('        case 0x18:')
        pickup = receiver[start:receiver.index('        case 0x19:', start)]
        field_items = prepared_source('code_0805D094.c')
        start = field_items.index('void *sub_0805D460(')
        lookup = field_items[start:field_items.index('/* [FindFieldItemByIdInStateTwo]', start)]
        harness = r'''
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

static u32 field[16 * 8], queue[50 * 34];
static u8 *gUnk_03005D58 = (u8 *)field;
static u8 *gRxQueue = (u8 *)queue;
static u32 gRxQueueCount;
static unsigned sub_080020F4(void) { return 0; }
#define x_sub_0805D460 sub_0805D460

@LOOKUP@

static void request(u32 peer, u16 id)
{
    u32 d[] = {id, 0};
    switch (0x99 - 0x81) {
@PICKUP@
    }
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
    const u16 id = 0x1234;
    const int missing = argc > 1 && argv[1][0] == 'm';
    u16 *flags = (u16 *)(gUnk_03005D58 + 10);
    if (!missing) {
        *flags = 1;
        *(u16 *)(gUnk_03005D58 + 12) = id;
        request(1, id);
        CHECK(gRxQueueCount == 1 && *flags == 2);
        CHECK(gRxQueue[0] == 0x9A && gRxQueue[1] == 1);
        CHECK(queue[1] == 1 && queue[2] == ((1u << 16) | 1) && queue[3] == id);
    }
    u8 *reply = gRxQueue + gRxQueueCount * 0x88;
    request(2, id);
    CHECK(gRxQueueCount == (missing ? 1 : 2));
    CHECK(*flags == (missing ? 0 : 2));
    CHECK(reply[0] == 0x9A && reply[1] == 1);
    CHECK(*(u32 *)(reply + 4) == 1 && *(u32 *)(reply + 8) == 2u << 16);
    CHECK(*(u32 *)(reply + 12) == id);
    return 0;
}
'''
        harness = harness.replace('@LOOKUP@', lookup).replace('@PICKUP@', pickup)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'field_item_pickup.c'
            exe = Path(directory) / 'field_item_pickup.exe'
            source.write_text(harness)
            subprocess.run([build.clang(), '-std=c11', str(source), '-o', str(exe)], check=True, cwd=ROOT)
            for scenario in ('missing', 'claimed'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(exe), scenario], check=True)
