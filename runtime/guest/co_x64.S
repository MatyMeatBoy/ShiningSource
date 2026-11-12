/* Coroutine switch for the game image (x86-64, System V, 32-bit pointers).
 * ss_co_swap(from, to): save the callee-saved registers and stack pointer in *from, load them from *to. */
        .text
        .globl ss_co_swap
        .type ss_co_swap, @function
ss_co_swap:
        pushq %rbp
        pushq %rbx
        pushq %r12
        pushq %r13
        pushq %r14
        pushq %r15
        movl %edi, %edi
        movl %esi, %esi
        movq %rsp, (%rdi)
        movq (%rsi), %rsp
        popq %r15
        popq %r14
        popq %r13
        popq %r12
        popq %rbx
        popq %rbp
        ret

/* First activation of a coroutine: the prepared stack returns here with the entry in r12 and its argument in r13. */
        .globl ss_co_start
        .type ss_co_start, @function
ss_co_start:
        subq $8, %rsp           /* the stack must be 16-byte aligned at a call */
        movl %r13d, %edi
        callq *%r12
        ud2
        .section .note.GNU-stack,"",@progbits
