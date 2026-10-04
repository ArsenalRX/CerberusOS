/* The first instructions of every program. The kernel enters here with the
   stack pointer at argc (see kernel/proc/process.cpp for the layout) and
   every other register zero. */
    .text
    .globl _start
    .hidden _start
    .type _start, @function
_start:
    xorl %ebp, %ebp             /* end of the frame chain */
    movq %rsp, %rdi             /* the initial stack block */
    andq $-16, %rsp
    call __libc_start           /* never returns */
    ud2
    .size _start, . - _start
    .section .note.GNU-stack, "", @progbits
