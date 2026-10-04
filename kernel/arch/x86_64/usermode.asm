; usermode.asm: the two doors between ring 0 and ring 3 that are not ordinary
; interrupts: the first entry into user mode, and the system-call fast path.
; Both keep the GS convention of percpu.h: swapgs on the way in from ring 3
; and again on the way out.

bits 64
section .text

extern syscall_dispatch

USER_CS equ 0x2B
USER_SS equ 0x23

%macro PUSH_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

; [[noreturn]] void enter_user(const InterruptFrame* frame)
; Loads every register from the frame and drops to ring 3 with iretq. The
; frame must describe user mode (user cs/ss, a user rip and rsp, IF set).
global enter_user
enter_user:
    cli
    mov rsp, rdi
    POP_REGS
    add rsp, 16                 ; vector + error code
    swapgs
    iretq

; System-call entry (LSTAR). On arrival: rcx = user rip, r11 = user rflags,
; rax = call number, arguments in rdi rsi rdx r10 r8 r9, interrupts off, and
; the stack is still the user's. An InterruptFrame is built on the thread's
; kernel stack so the rest of the kernel sees the same layout as for an
; interrupt, and so fork and execve can copy or rewrite the whole user state.
global syscall_entry
syscall_entry:
    swapgs
    mov [gs:8], rsp             ; PerCpu.user_rsp
    mov rsp, [gs:0]             ; PerCpu.kernel_rsp
    push qword USER_SS
    push qword [gs:8]
    push r11                    ; rflags
    push qword USER_CS
    push rcx                    ; rip
    push qword 0                ; error code slot
    push qword 0                ; vector slot
    PUSH_REGS
    mov rdi, rsp
    sti                         ; system calls are preemptible
    call syscall_dispatch
    cli
    POP_REGS
    add rsp, 16
    ; syscall_dispatch has checked that rip is a canonical user address and
    ; has cleaned the flags; sysret would fault in ring 0 otherwise.
    mov rcx, [rsp]              ; rip
    mov r11, [rsp + 16]         ; rflags
    mov rsp, [rsp + 24]         ; user stack; interrupts are off until sysret
    swapgs
    o64 sysret
