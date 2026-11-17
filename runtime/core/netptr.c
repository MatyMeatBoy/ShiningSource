/* The game's scene-task messages contain a code pointer in payload word0.
 * Keep native pointers out of the network/order/snapshot queues. Other payload
 * words are values, not addresses to relocate. */
#include "shiningsource/runtime.h"

int ss_net_code_message(void *message, int size, int receiving)
{
    uint8_t *bytes = message;
    uint32_t value, converted;
    int expected;
    if (!bytes || size < 1)
        return 0;
    if (bytes[0] != 0xA3 && bytes[0] != 0xA4)
        return 1;
    expected = bytes[0] == 0xA3 ? 10 : 18;
    if (size != expected || bytes[1] != (expected - 2) / 8)
        return 0;
    value = bytes[2] | (uint32_t)bytes[3] << 8 | (uint32_t)bytes[4] << 16 | (uint32_t)bytes[5] << 24;
    if (!(receiving ? ss_code_decode(value, &converted) : ss_code_encode(value, &converted)))
        return 0;
    bytes[2] = (uint8_t)converted;
    bytes[3] = (uint8_t)(converted >> 8);
    bytes[4] = (uint8_t)(converted >> 16);
    bytes[5] = (uint8_t)(converted >> 24);
    return 1;
}
