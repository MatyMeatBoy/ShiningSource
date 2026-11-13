/* Stray memory accesses.
 *
 * On the console, reading or writing an address with nothing mapped does not stop the program: writes are
 * dropped and reads return a value. Cartridges contain such accesses (a write through a pointer that is still
 * NULL, for example) and work anyway. A PC process faults instead, so the fault handler calls this: it decodes
 * the faulting x86 instruction, drops the write or gives the read a zero, and continues after it.
 *
 * Only the instruction forms compilers use for loads and stores are handled. Anything else is reported as a
 * real crash, with its opcode, so the list can grow from evidence.
 */
#include "shiningsource/runtime.h"

#include <stdio.h>
#include <windows.h>

static unsigned long tolerated;

static DWORD *reg32(CONTEXT *c, int index)
{
    switch (index & 7) {
    case 0: return &c->Eax; case 1: return &c->Ecx; case 2: return &c->Edx; case 3: return &c->Ebx;
    case 4: return &c->Esp; case 5: return &c->Ebp; case 6: return &c->Esi; default: return &c->Edi;
    }
}

static void clear_reg8(CONTEXT *c, int index)
{
    DWORD *full = reg32(c, index & 3);
    if (index & 4) *full &= ~0xFF00u;       /* AH CH DH BH */
    else *full &= ~0xFFu;                   /* AL CL DL BL */
}

/* Set the arithmetic flags for a result of `bits` bits (overflow cleared, carry as given). */
static void value_flags(CONTEXT *c, uint32_t value, int bits, int carry)
{
    uint32_t sign = 1u << (bits - 1);
    c->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);     /* CF ZF SF OF */
    if (carry) c->EFlags |= 0x0001u;
    if (value == 0) c->EFlags |= 0x0040u;
    if (value & sign) c->EFlags |= 0x0080u;
}

/* Length of a ModRM memory operand starting at `p` (ModRM byte included); 0 if it is a register operand. */
static int modrm_length(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, length = 1;
    if (mod == 3)
        return 0;
    if (rm == 4) {
        length++;
        if (mod == 0 && (p[1] & 7) == 5)
            length += 4;
    } else if (mod == 0 && rm == 5) {
        length += 4;
    }
    if (mod == 1) length += 1;
    if (mod == 2) length += 4;
    return length;
}

unsigned long ss_stray_count(void)
{
    return tolerated;
}

/* Returns 1 if the access was absorbed and execution can continue. */
int ss_stray_access(void *context, uint32_t address, int is_write)
{
    CONTEXT *c = context;
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->Eip;
    int operand16 = 0, length, reg;
    static uint32_t reported[64];
    static int reported_count;
    int i;

    /* Only addresses the console would not have: outside every mapped region. */
    if (address >= SS_EWRAM && address < SS_ROM + 0x1000000)
        return 0;
    while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36 || *p == 0x64 || *p == 0x65) {
        if (*p == 0x66)
            operand16 = 1;
        p++;
    }
    if (p[0] == 0x0F) {
        length = modrm_length(p + 2);
        reg = (p[2] >> 3) & 7;
        if (!length)
            return 0;
        switch (p[1]) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:         /* movzx / movsx r, m */
            *reg32(c, reg) = 0;
            break;
        case 0x11: case 0x29: case 0x7F: case 0xD6: case 0x13: case 0x17:   /* vector stores */
            if (!is_write) return 0;
            break;
        default:
            return 0;
        }
        p += 2 + length;
    } else {
        length = modrm_length(p + 1);
        reg = (p[1] >> 3) & 7;
        if (!length)
            return 0;
        switch (p[0]) {
        case 0x88: case 0x89:                               /* mov m, r */
            p += 1 + length;
            break;
        case 0xC6:                                          /* mov m8, imm8 */
            p += 1 + length + 1;
            break;
        case 0xC7:                                          /* mov m, imm */
            p += 1 + length + (operand16 ? 2 : 4);
            break;
        case 0x8A:                                          /* mov r8, m8 */
            clear_reg8(c, reg);
            p += 1 + length;
            break;
        case 0x8B:                                          /* mov r, m */
            if (operand16) *reg32(c, reg) &= ~0xFFFFu;
            else *reg32(c, reg) = 0;
            p += 1 + length;
            break;
        case 0x00: case 0x01: case 0x08: case 0x09: case 0x20: case 0x21: case 0x28: case 0x29: case 0x30: case 0x31:
            p += 1 + length;                                /* op m, r: the store is dropped */
            break;
        case 0x80: case 0x83: case 0x81: {                  /* op m, imm: stores are dropped; cmp is 0 - imm */
            int wide = p[0] == 0x81 ? (operand16 ? 2 : 4) : 1, bits = p[0] == 0x80 ? 8 : (operand16 ? 16 : 32);
            if (reg == 7) {
                const uint8_t *imm = p + 1 + length;
                uint32_t value = wide == 1 ? (uint32_t)(int32_t)(int8_t)imm[0] : wide == 2 ? (imm[0] | imm[1] << 8)
                                 : (imm[0] | imm[1] << 8 | imm[2] << 16 | (uint32_t)imm[3] << 24);
                uint32_t mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
                value_flags(c, (0u - value) & mask, bits, (value & mask) != 0);
            }
            p += 1 + length + wide;
            break;
        }
        case 0xFE: case 0xFF:                               /* inc / dec m only */
            if (reg > 1) return 0;
            p += 1 + length;
            break;
        /* op r, m with the memory operand read as zero */
        case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x2A: case 0x2B: case 0x32: case 0x33:
            value_flags(c, p[0] & 1 ? (operand16 ? *reg32(c, reg) & 0xFFFF : *reg32(c, reg)) : *reg32(c, reg & 3) & 0xFF,
                        p[0] & 1 ? (operand16 ? 16 : 32) : 8, 0);       /* add/or/sub/xor with 0: value unchanged */
            p += 1 + length;
            break;
        case 0x22:                                          /* and r8, m8 */
            clear_reg8(c, reg);
            value_flags(c, 0, 8, 0);
            p += 1 + length;
            break;
        case 0x23:                                          /* and r, m */
            if (operand16) *reg32(c, reg) &= ~0xFFFFu;
            else *reg32(c, reg) = 0;
            value_flags(c, 0, 32, 0);
            p += 1 + length;
            break;
        case 0x3A: case 0x3B:                               /* cmp r, m: r - 0 */
            value_flags(c, p[0] & 1 ? (operand16 ? *reg32(c, reg) & 0xFFFF : *reg32(c, reg)) : *reg32(c, reg & 3) & 0xFF,
                        p[0] & 1 ? (operand16 ? 16 : 32) : 8, 0);
            p += 1 + length;
            break;
        case 0x38: case 0x39: {                             /* cmp m, r: 0 - r */
            uint32_t r = p[0] & 1 ? (operand16 ? *reg32(c, reg) & 0xFFFF : *reg32(c, reg)) : *reg32(c, reg & 3) & 0xFF;
            int bits = p[0] & 1 ? (operand16 ? 16 : 32) : 8;
            value_flags(c, (0u - r) & (bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1), bits, r != 0);
            p += 1 + length;
            break;
        }
        case 0x84: case 0x85:                               /* test m, r: 0 & r */
            value_flags(c, 0, 32, 0);
            p += 1 + length;
            break;
        case 0xF6: case 0xF7:                               /* test m, imm only */
            if (reg != 0) return 0;
            value_flags(c, 0, 32, 0);
            p += 1 + length + (p[0] == 0xF6 ? 1 : (operand16 ? 2 : 4));
            break;
        default:
            return 0;
        }
    }
    tolerated++;
    for (i = 0; i < reported_count; i++)
        if (reported[i] == c->Eip)
            break;
    if (i == reported_count && reported_count < 64) {
        reported[reported_count++] = c->Eip;
        fprintf(stderr, "stray %s of %08lX at eip %08lX tolerated, as on the console\n", is_write ? "write" : "read",
                (unsigned long)address, (unsigned long)c->Eip);
        fflush(stderr);
    }
    c->Eip = (DWORD)(uintptr_t)p;
    return 1;
}

/* Division that traps on a PC: by zero, or the most negative number by -1. The cartridge divides with the
 * compiler's library routines, which return instead of trapping; their results (read from the ROM's own
 * routines) are quotient 0 and remainder 0 for a zero divisor, and the dividend with remainder 0 for the
 * overflow. Every build must give those numbers: here by continuing after the instruction with them, on
 * processors that do not trap by rewriting the division before it is compiled (tools/division_ir.py).
 * Returns 1 if handled. */
int ss_divide_by_zero(void *context)
{
    CONTEXT *c = context;
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->Eip;
    int length, wide;

    while (*p == 0x66 || *p == 0xF2 || *p == 0xF3)
        p++;
    if (p[0] != 0xF7 && p[0] != 0xF6)
        return 0;
    if (((p[1] >> 3) & 7) < 6)              /* only div and idiv */
        return 0;
    wide = p[0] == 0xF7 && p == (const uint8_t *)(uintptr_t)c->Eip;     /* a plain 32-bit divide */
    length = modrm_length(p + 1);
    if (!length)
        length = 1;                         /* register operand */
    if (wide && ((p[1] >> 3) & 7) == 7 && (p[1] >> 6) == 3 && *reg32(c, p[1] & 7) == 0xFFFFFFFFu) {
        c->Edx = 0;                         /* overflow: the quotient register already holds the dividend */
    } else {
        c->Eax = 0;
        c->Edx = 0;
    }
    c->Eip = (DWORD)(uintptr_t)(p + 1 + length);
    tolerated++;
    return 1;
}
