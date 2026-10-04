; usercopy_stubs.asm: the instructions that touch user memory, and their
; fault fixups. g_smap_enabled (arch/x86_64/cpufeatures.cpp) is 1 when SMAP is
; on; stac/clac are only
; valid instructions on CPUs that have it.

bits 64
section .text

extern g_smap_enabled

global usercopy_raw, usercopy_insn, usercopy_fixup_target
global userstr_raw, userstr_insn, userstr_fixup_target
global userclear_raw, userclear_insn, userclear_fixup_target

%macro OPEN_USER 0
    cmp byte [rel g_smap_enabled], 0
    je %%skip
    stac
%%skip:
%endmacro

%macro CLOSE_USER 0
    cmp byte [rel g_smap_enabled], 0
    je %%skip
    clac
%%skip:
%endmacro

; int usercopy_raw(void* dst, const void* src, usize n)   0 = copied, 1 = faulted
usercopy_raw:
    mov rcx, rdx
    OPEN_USER
usercopy_insn:
    rep movsb
    CLOSE_USER
    xor eax, eax
    ret
usercopy_fixup_target:
    CLOSE_USER
    mov eax, 1
    ret

; long userstr_raw(char* dst, const char* src, usize max)
; Copies up to and including the NUL. Returns the length without the NUL,
; -1 on a fault, -2 if max bytes held no NUL.
userstr_raw:
    xor eax, eax
    OPEN_USER
userstr_next:
    cmp rax, rdx
    jae userstr_too_long
userstr_insn:
    mov cl, [rsi + rax]
    mov [rdi + rax], cl
    test cl, cl
    jz userstr_done
    inc rax
    jmp userstr_next
userstr_done:
    CLOSE_USER
    ret
userstr_too_long:
    CLOSE_USER
    mov rax, -2
    ret
userstr_fixup_target:
    CLOSE_USER
    mov rax, -1
    ret

; int userclear_raw(void* dst, usize n)   0 = cleared, 1 = faulted
userclear_raw:
    mov rcx, rsi
    xor eax, eax
    OPEN_USER
userclear_insn:
    rep stosb
    CLOSE_USER
    xor eax, eax
    ret
userclear_fixup_target:
    CLOSE_USER
    mov eax, 1
    ret
