// The anti-aliased fonts (kernel/gfx/fonts.bin, made by tools/gen-fonts.py)
// embedded in the window server, as the kernel embeds them in font.S. The
// path is resolved through the assembler include path (-Wa,-I<repo root>).
    .section .rodata
    .global _binary_fonts_aa_start
    .global _binary_fonts_aa_end
    .balign 16
_binary_fonts_aa_start:
    .incbin "kernel/gfx/fonts.bin"
_binary_fonts_aa_end:
    .section .note.GNU-stack,"",@progbits
