; isr_stubs.asm: interrupt entry stubs for all 256 vectors, the common register save/restore
; path, and capture_registers() used by PANIC. Every stub normalises the stack
; to an InterruptFrame (see interrupts.h) and calls interrupt_dispatch.

bits 64
section .text

extern interrupt_dispatch

%macro ISR_NOERR 1
isr_%1:
    push qword 0                ; fake error code so the frame is uniform
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    push qword %1
    jmp isr_common
%endmacro

isr_common:
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
    mov rdi, rsp
    cld
    call interrupt_dispatch
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
    add rsp, 16                 ; vector + error code
    iretq

; Vectors that push a hardware error code: #DF #TS #NP #SS #GP #PF #AC #CP #VC #SX
%assign i 0
%rep 256
    %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
        ISR_ERR i
    %else
        ISR_NOERR i
    %endif
%assign i i+1
%endrep

; void capture_registers(InterruptFrame* out)
; Snapshots the caller's general registers, rip (return address), rflags and
; rsp so PANIC can print a register dump without a trap.
global capture_registers
capture_registers:
    mov [rdi + 0*8], r15
    mov [rdi + 1*8], r14
    mov [rdi + 2*8], r13
    mov [rdi + 3*8], r12
    mov [rdi + 4*8], r11
    mov [rdi + 5*8], r10
    mov [rdi + 6*8], r9
    mov [rdi + 7*8], r8
    mov [rdi + 8*8], rbp
    mov [rdi + 9*8], rdi
    mov [rdi + 10*8], rsi
    mov [rdi + 11*8], rdx
    mov [rdi + 12*8], rcx
    mov [rdi + 13*8], rbx
    mov [rdi + 14*8], rax
    mov qword [rdi + 15*8], 0           ; vector
    mov qword [rdi + 16*8], 0           ; error
    mov rax, [rsp]
    mov [rdi + 17*8], rax               ; rip = return address
    mov ax, cs
    movzx rax, ax
    mov [rdi + 18*8], rax
    pushfq
    pop rax
    mov [rdi + 19*8], rax               ; rflags
    lea rax, [rsp + 8]
    mov [rdi + 20*8], rax               ; rsp before the call
    mov ax, ss
    movzx rax, ax
    mov [rdi + 21*8], rax
    ret

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_%+i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
