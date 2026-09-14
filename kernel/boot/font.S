// Embeds the PSF2 console font as a read-only blob. The path is resolved via the
// assembler include path (-Wa,-I<repo root>), see the Makefile.
    .section .rodata
    .global _binary_font_8x16_start
    .global _binary_font_8x16_end
    .balign 16
_binary_font_8x16_start:
    .incbin "kernel/boot/font/terminus-8x16.psf"
_binary_font_8x16_end:
    .section .note.GNU-stack,"",@progbits
