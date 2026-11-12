/* Coroutine switch for the game image on AArch64 with 32-bit pointers (arm64_32, Mach-O).
 * ss_co_swap(from, to): save the callee-saved registers and stack pointer in *from, load them from *to. */
        .text
        .globl _ss_co_swap
        .p2align 2
_ss_co_swap:
        mov w0, w0
        mov w1, w1
        sub sp, sp, #176
        stp x19, x20, [sp, #0]
        stp x21, x22, [sp, #16]
        stp x23, x24, [sp, #32]
        stp x25, x26, [sp, #48]
        stp x27, x28, [sp, #64]
        stp x29, x30, [sp, #80]
        stp d8, d9, [sp, #96]
        stp d10, d11, [sp, #112]
        stp d12, d13, [sp, #128]
        stp d14, d15, [sp, #144]
        mov x9, sp
        str x9, [x0]
        ldr x9, [x1]
        mov sp, x9
        ldp x19, x20, [sp, #0]
        ldp x21, x22, [sp, #16]
        ldp x23, x24, [sp, #32]
        ldp x25, x26, [sp, #48]
        ldp x27, x28, [sp, #64]
        ldp x29, x30, [sp, #80]
        ldp d8, d9, [sp, #96]
        ldp d10, d11, [sp, #112]
        ldp d12, d13, [sp, #128]
        ldp d14, d15, [sp, #144]
        add sp, sp, #176
        ret

/* First activation of a coroutine: the prepared frame returns here with the argument in x19 and the entry in x20. */
        .globl _ss_co_start
        .p2align 2
_ss_co_start:
        mov w0, w19
        mov x29, #0
        blr x20
        brk #1
