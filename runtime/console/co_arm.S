/* Coroutine switch for the game image on 32-bit ARM (PS Vita: ARMv7, Thumb-2, hard float).
 * ss_co_swap(from, to): save the callee-saved registers and stack pointer in *from, load them from *to.
 * The frame is d8..d15 (64 bytes), r4..r11 and the return address; runtime/guest/guest.c prepares the same
 * frame for a new coroutine. */
        .syntax unified
        .text
        .thumb
        .globl ss_co_swap
        .type ss_co_swap, %function
        .thumb_func
        .p2align 2
ss_co_swap:
        push {r4-r11, lr}
        vpush {d8-d15}
        mov r2, sp
        str r2, [r0]
        ldr r2, [r1]
        mov sp, r2
        vpop {d8-d15}
        pop {r4-r11, pc}

/* First activation of a coroutine: the prepared frame returns here with the argument in r4 and the entry in r5. */
        .globl ss_co_start
        .type ss_co_start, %function
        .thumb_func
        .p2align 2
ss_co_start:
        mov r0, r4
        blx r5
        udf #1
