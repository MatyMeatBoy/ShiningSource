/* Frame boundary: what happens when the game waits for the vertical blank.
 *
 * The cartridge's interrupt dispatcher (IntrMain in crt0.s) looks the pending interrupt up in a table of 14
 * handlers that the game fills. Here the table is read directly and the handler is a native function.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <stdlib.h>

#define REG16(offset) (*(volatile uint16_t *)(uintptr_t)(SS_IO + (offset)))
#define REG_DISPSTAT REG16(0x004)
#define REG_VCOUNT   REG16(0x006)
#define REG_KEYINPUT REG16(0x130)
#define REG_IE       REG16(0x200)
#define REG_IF       REG16(0x202)
#define REG_IME      REG16(0x208)

/* order used by the dispatcher */
static const uint16_t dispatch_order[14] = {
    0x0004, 0x0002, 0x00C0, 0x0800, 0x0100, 0x0001, 0x0008, 0x0010, 0x0020, 0x0040, 0x0200, 0x0400, 0x1000, 0x2000,
};
static uint32_t *const handler_table = (uint32_t *)(uintptr_t)0x030012A0u;

static unsigned long frame_count;
static unsigned long frame_limit;

uint32_t ss_frame_hash(void)
{
    static const struct { uint32_t base, size; } parts[] = {
        { SS_IO, 0x60 }, { SS_PALETTE, 0x400 }, { SS_VRAM, 0x18000 }, { SS_OAM, 0x400 },
    };
    uint32_t hash = 2166136261u;
    unsigned i, j;
    for (i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)parts[i].base;
        for (j = 0; j < parts[i].size; j++)
            hash = (hash ^ bytes[j]) * 16777619u;
    }
    return hash;
}

void ss_raise_interrupt(uint16_t flag)
{
    unsigned i;
    if (!(REG_IME & 1) || !(REG_IE & flag))
        return;
    REG_IF |= flag;
    for (i = 0; i < 14; i++) {
        if (dispatch_order[i] & flag) {
            uint32_t handler = handler_table[i];
            if (handler)
                ((void (*)(void))(uintptr_t)handler)();
            return;
        }
    }
}

void VBlankIntrWait(void)
{
    const char *limit;

    if (frame_count == 0) {
        limit = getenv("SS_FRAMES");
        frame_limit = limit ? strtoul(limit, NULL, 10) : 0;
        REG_KEYINPUT = 0x03FF;          /* no button held */
    }
    REG_VCOUNT = 160;
    REG_DISPSTAT |= 1;                  /* in vertical blank */
    if (REG_DISPSTAT & 0x0008)
        ss_raise_interrupt(0x0001);
    frame_count++;
    {
        static unsigned long dump_at = ~0ul;
        if (dump_at == ~0ul) {
            const char *text = getenv("SS_DUMP");       /* SS_DUMP=N writes the video state of frame N */
            dump_at = text ? strtoul(text, NULL, 10) : 0;
        }
        if (dump_at && frame_count == dump_at) {
            FILE *file = fopen("ss_video_dump.bin", "wb");
            if (file) {
                fwrite((void *)(uintptr_t)SS_IO, 1, 0x400, file);
                fwrite((void *)(uintptr_t)SS_PALETTE, 1, 0x400, file);
                fwrite((void *)(uintptr_t)SS_VRAM, 1, 0x18000, file);
                fwrite((void *)(uintptr_t)SS_OAM, 1, 0x400, file);
                fclose(file);
            }
        }
    }
    if (frame_limit) {
        if (frame_count <= 5 || frame_count % 60 == 0 || frame_count == frame_limit)
            fprintf(stderr, "frame %5lu  hash %08X  stubs reached %d\n", frame_count, (unsigned)ss_frame_hash(), ss_stub_count());
        if (frame_count >= frame_limit)
            exit(0);
    }
    REG_DISPSTAT &= ~1;
    REG_VCOUNT = 0;
}
