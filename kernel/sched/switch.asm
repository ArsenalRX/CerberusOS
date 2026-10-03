; switch.asm: the context switch and the first instructions of a new thread.
; Only the callee-saved registers are saved: a thread is always switched out
; inside a call to context_switch, so the caller-saved ones are already dead.
; The kernel uses no FPU/SSE state, so there is none to save.

bits 64
section .text

extern thread_bootstrap

; void context_switch(u64* save_rsp, u64 load_rsp)
; Saves the current thread's registers on its stack and its stack pointer in
; *save_rsp, then resumes the thread whose stack pointer is load_rsp.
; Interrupts must be disabled.
global context_switch
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First return address of every new thread. kthread_create builds a frame
; that leaves the entry function in r12 and its argument in r13, with rbp = 0
; so backtraces end here.
global thread_start
thread_start:
    mov rdi, r12
    mov rsi, r13
    call thread_bootstrap       ; never returns
    ud2
