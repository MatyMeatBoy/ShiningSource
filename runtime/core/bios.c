/* GBA BIOS calls used by the games, in C. Behaviour follows the documented BIOS (GBATEK). */
#include "shiningsource/runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(address) ((void *)(uintptr_t)(address))

int32_t Div(int32_t numerator, int32_t denominator)
{
    return denominator ? numerator / denominator : 0;
}

uint16_t Sqrt(uint32_t value)
{
    uint32_t root = 0, bit = 1u << 30;
    while (bit > value)
        bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)root;
}

/* BIOS ArcTan: polynomial in 1.14 fixed point; ArcTan2 picks the octant. Result: 0..0xFFFF for a full turn. */
static int32_t bios_arctan(int32_t i)
{
    int32_t a = -((i * i) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

uint16_t ArcTan2(int16_t x, int16_t y)
{
    if (y == 0)
        return x >= 0 ? 0 : 0x8000;
    if (x == 0)
        return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y)
                return (uint16_t)bios_arctan((y << 14) / x);
        } else if (-x >= y) {
            return (uint16_t)(bios_arctan((y << 14) / x) + 0x8000);
        }
        return (uint16_t)(0x4000 - bios_arctan((x << 14) / y));
    }
    if (x <= 0) {
        if (-x > -y)
            return (uint16_t)(bios_arctan((y << 14) / x) + 0x8000);
    } else if (x >= -y) {
        return (uint16_t)(bios_arctan((y << 14) / x) + 0x10000);
    }
    return (uint16_t)(0xC000 - bios_arctan((x << 14) / y));
}

void CpuSet(const void *source, void *destination, uint32_t control)
{
    uint32_t count = control & 0x1FFFFF, i;
    int fill = (control >> 24) & 1;
    if (control & (1u << 26)) {
        const uint32_t *s = source;
        uint32_t *d = destination;
        for (i = 0; i < count; i++)
            d[i] = fill ? s[0] : s[i];
    } else {
        const uint16_t *s = source;
        uint16_t *d = destination;
        for (i = 0; i < count; i++)
            d[i] = fill ? s[0] : s[i];
    }
}

void CpuFastSet(const void *source, void *destination, uint32_t control)
{
    uint32_t count = ((control & 0x1FFFFF) + 7) & ~7u, i;
    const uint32_t *s = source;
    uint32_t *d = destination;
    int fill = (control >> 24) & 1;
    for (i = 0; i < count; i++)
        d[i] = fill ? s[0] : s[i];
}

void LZ77UnCompWram(const uint8_t *source, uint8_t *destination)
{
    uint32_t size = source[1] | source[2] << 8 | source[3] << 16, out = 0;
    int bit;
    source += 4;
    while (out < size) {
        uint8_t flags = *source++;
        for (bit = 0; bit < 8 && out < size; bit++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t count = (source[0] >> 4) + 3, back = ((source[0] & 15) << 8 | source[1]) + 1;
                source += 2;
                while (count-- && out < size) {
                    destination[out] = destination[out - back];
                    out++;
                }
            } else {
                destination[out++] = *source++;
            }
        }
    }
}

void LZ77UnCompVram(const uint8_t *source, uint8_t *destination)
{
    LZ77UnCompWram(source, destination);
}

/* Scale/rotate parameters for sprites: sx, sy in 8.8, angle in the top 8 bits of a 16-bit turn. */
void ObjAffineSet(const void *source, void *destination, int32_t count, int32_t offset)
{
    const int16_t *in = source;
    uint8_t *out = destination;
    while (count-- > 0) {
        double angle = (double)((uint16_t)in[2] >> 8) * (3.14159265358979323846 / 128.0);
        int32_t sine = (int32_t)lround(sin(angle) * 16384.0), cosine = (int32_t)lround(cos(angle) * 16384.0);
        int32_t sx = in[0], sy = in[1];
        *(int16_t *)(out) = (int16_t)((sx * cosine) >> 14);
        *(int16_t *)(out + offset) = (int16_t)(-(sx * sine) >> 14);
        *(int16_t *)(out + offset * 2) = (int16_t)((sy * sine) >> 14);
        *(int16_t *)(out + offset * 3) = (int16_t)((sy * cosine) >> 14);
        in += 4;
        out += offset * 4;
    }
}

void RegisterRamReset(uint32_t flags)
{
    if (flags & 0x01) memset(P(SS_EWRAM), 0, 0x40000);
    if (flags & 0x02) memset(P(SS_IWRAM), 0, 0x7E00);
    if (flags & 0x04) memset(P(SS_PALETTE), 0, 0x400);
    if (flags & 0x08) memset(P(SS_VRAM), 0, 0x18000);
    if (flags & 0x10) memset(P(SS_OAM), 0, 0x400);
    if (flags & 0x20) memset(P(SS_IO + 0x120), 0, 0x20);
    if (flags & 0x40) memset(P(SS_IO + 0x60), 0, 0x50);
    if (flags & 0x80) {
        memset(P(SS_IO), 0, 0x60);
        memset(P(SS_IO + 0xB0), 0, 0x70);
        memset(P(SS_IO + 0x200), 0, 0x10);
    }
}

void SoftReset(uint32_t flags)
{
    fprintf(stderr, "SoftReset(%u) called by the game\n", (unsigned)flags);
    exit(0);
}
