/* Accesses the console would have ignored, for x86-64 hosts (the reasoning is in win32/stray.c).
 *
 * The operating-system part of each host fills a struct ss_cpu with pointers into the saved context of the
 * fault and calls ss_stray_access64 / ss_divide_by_zero64. Shared by the Windows and Linux hosts.
 */
#ifndef SS_STRAY64_H
#define SS_STRAY64_H

#include <stdint.h>

struct ss_cpu {
    uint64_t *reg[16];      /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15 */
    uint64_t *rip;
    uint64_t flags;         /* in/out copy of the flags register */
};

static unsigned long ss_tolerated;

static int ss_modrm_length(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, length = 1;
    if (mod == 3) return 0;
    if (rm == 4) {
        length++;
        if (mod == 0 && (p[1] & 7) == 5) length += 4;
    } else if (mod == 0 && rm == 5) {
        length += 4;
    }
    if (mod == 1) length += 1;
    if (mod == 2) length += 4;
    return length;
}

static void ss_value_flags(struct ss_cpu *c, uint64_t value, int bits, int carry)
{
    uint64_t sign = 1ull << (bits - 1);
    c->flags &= ~(uint64_t)(0x0001u | 0x0040u | 0x0080u | 0x0800u);     /* CF ZF SF OF */
    if (carry) c->flags |= 0x0001u;
    if (value == 0) c->flags |= 0x0040u;
    if (value & sign) c->flags |= 0x0080u;
}

/* Returns 1 if the access was absorbed: the write is dropped or the read gives zero, and rip moves on. */
static int ss_stray_access64(struct ss_cpu *c, uint64_t address, int is_write, uint64_t mapped_low, uint64_t mapped_high)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)*c->rip;
    int operand16 = 0, rex = 0, length, reg, bits;
    uint64_t mask, value;

    if (address >= mapped_low && address < mapped_high)
        return 0;
    while (*p == 0x66 || *p == 0x67 || *p == 0xF2 || *p == 0xF3 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36 || *p == 0x64 || *p == 0x65) {
        if (*p == 0x66) operand16 = 1;
        p++;
    }
    if ((*p & 0xF0) == 0x40)
        rex = *p++;
    bits = (rex & 8) ? 64 : operand16 ? 16 : 32;
    mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    if (p[0] == 0x0F) {
        length = ss_modrm_length(p + 2);
        reg = ((p[2] >> 3) & 7) | ((rex & 4) << 1);
        if (!length) return 0;
        switch (p[1]) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: *c->reg[reg] = 0; break;
        case 0x11: case 0x29: case 0x7F: case 0xD6: case 0x13: case 0x17: if (!is_write) return 0; break;
        default: return 0;
        }
        p += 2 + length;
    } else {
        length = ss_modrm_length(p + 1);
        reg = ((p[1] >> 3) & 7) | ((rex & 4) << 1);
        if (!length) return 0;
        value = *c->reg[reg] & ((p[0] & 1) ? mask : 0xFF);
        switch (p[0]) {
        case 0x88: case 0x89: p += 1 + length; break;
        case 0xC6: p += 1 + length + 1; break;
        case 0xC7: p += 1 + length + (operand16 ? 2 : 4); break;
        case 0x8A: *c->reg[reg] &= ~0xFFull; p += 1 + length; break;
        case 0x8B:
            if (operand16) *c->reg[reg] &= ~0xFFFFull;
            else *c->reg[reg] = 0;                          /* a 32-bit load clears the upper half too */
            p += 1 + length;
            break;
        case 0x63: *c->reg[reg] = 0; p += 1 + length; break;            /* movsxd r64, m32 */
        case 0x00: case 0x01: case 0x08: case 0x09: case 0x20: case 0x21: case 0x28: case 0x29: case 0x30: case 0x31:
            p += 1 + length;
            break;
        case 0x80: case 0x83: case 0x81: {
            int wide = p[0] == 0x81 ? (operand16 ? 2 : 4) : 1, opbits = p[0] == 0x80 ? 8 : bits;
            if (((p[1] >> 3) & 7) == 7) {
                const uint8_t *imm = p + 1 + length;
                uint64_t operand = wide == 1 ? (uint64_t)(int64_t)(int8_t)imm[0] : wide == 2 ? (uint64_t)(imm[0] | imm[1] << 8)
                                   : (uint64_t)(int64_t)(int32_t)(imm[0] | imm[1] << 8 | imm[2] << 16 | (uint32_t)imm[3] << 24);
                uint64_t m = opbits == 64 ? ~0ull : (1ull << opbits) - 1;
                ss_value_flags(c, (0ull - operand) & m, opbits, (operand & m) != 0);
            }
            p += 1 + length + wide;
            break;
        }
        case 0xFE: case 0xFF:
            if (((p[1] >> 3) & 7) > 1) return 0;
            p += 1 + length;
            break;
        case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x2A: case 0x2B: case 0x32: case 0x33: case 0x3A: case 0x3B:
            ss_value_flags(c, value, (p[0] & 1) ? bits : 8, 0);
            p += 1 + length;
            break;
        case 0x22: *c->reg[reg] &= ~0xFFull; ss_value_flags(c, 0, 8, 0); p += 1 + length; break;
        case 0x23:
            if (operand16) *c->reg[reg] &= ~0xFFFFull; else *c->reg[reg] = 0;
            ss_value_flags(c, 0, 32, 0);
            p += 1 + length;
            break;
        case 0x38: case 0x39: {
            int opbits = (p[0] & 1) ? bits : 8;
            uint64_t m = opbits == 64 ? ~0ull : (1ull << opbits) - 1;
            ss_value_flags(c, (0ull - value) & m, opbits, value != 0);
            p += 1 + length;
            break;
        }
        case 0x84: case 0x85: ss_value_flags(c, 0, 32, 0); p += 1 + length; break;
        case 0xF6: case 0xF7:
            if (((p[1] >> 3) & 7) != 0) return 0;
            ss_value_flags(c, 0, 32, 0);
            p += 1 + length + (p[0] == 0xF6 ? 1 : (operand16 ? 2 : 4));
            break;
        default: return 0;
        }
    }
    ss_tolerated++;
    *c->rip = (uint64_t)(uintptr_t)p;
    return 1;
}

/* The cartridge divides with a routine that returns instead of trapping: skip the divide, results are zero. */
/* A trapping division: see ss_divide_by_zero in win32/stray.c for the rule (the ARM64 results). */
static int ss_divide_by_zero64(struct ss_cpu *c)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)*c->rip;
    int length, rex = 0, prefixed = 0, wide;
    while (*p == 0x66 || *p == 0x67 || *p == 0xF2 || *p == 0xF3) { prefixed |= *p == 0x66; p++; }
    if ((*p & 0xF0) == 0x40) rex = *p++;
    if ((p[0] != 0xF7 && p[0] != 0xF6) || ((p[1] >> 3) & 7) < 6)
        return 0;
    wide = p[0] == 0xF7 && !prefixed && !(rex & 8);         /* a plain 32-bit divide */
    length = ss_modrm_length(p + 1);
    if (!length) length = 1;
    if (wide && ((p[1] >> 3) & 7) == 7 && (p[1] >> 6) == 3 && (uint32_t)*c->reg[(p[1] & 7) | ((rex & 1) << 3)] == 0xFFFFFFFFu) {
        *c->reg[2] = 0;                     /* overflow: the quotient register already holds the dividend */
    } else if (wide) {
        *c->reg[2] = (uint32_t)*c->reg[0];
        *c->reg[0] = 0;
    } else {
        *c->reg[0] = 0;
        *c->reg[2] = 0;
    }
    *c->rip = (uint64_t)(uintptr_t)(p + 1 + length);
    ss_tolerated++;
    return 1;
}

#endif
