; probe_stubs.asm: the instructions that are allowed to fault (see probe.h).
; Each probe has one labelled faulting instruction and a fixup label the
; page-fault handler redirects to; the fixup returns 0 instead of 1.

bits 64
section .text

global probe_read_raw, probe_read_insn, probe_read_fixup
global probe_write_raw, probe_write_insn, probe_write_fixup
global probe_exec_raw, probe_exec_fixup

; int probe_read_raw(const void* addr, u8* out)
probe_read_raw:
    push rbp
    mov rbp, rsp
probe_read_insn:
    mov al, [rdi]
    mov [rsi], al
    mov eax, 1
    pop rbp
    ret
probe_read_fixup:
    xor eax, eax
    pop rbp
    ret

; int probe_write_raw(void* addr, u8 value)
probe_write_raw:
    push rbp
    mov rbp, rsp
probe_write_insn:
    mov [rdi], sil
    mov eax, 1
    pop rbp
    ret
probe_write_fixup:
    xor eax, eax
    pop rbp
    ret

; int probe_exec_raw(const void* addr)
probe_exec_raw:
    push rbp
    mov rbp, rsp
    call rdi
    mov eax, 1
    pop rbp
    ret
probe_exec_fixup:
    xor eax, eax
    pop rbp
    ret
