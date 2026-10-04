// Embeds the PSF2 console fonts as read-only blobs. Paths are resolved via the
// assembler include path (-Wa,-I<repo root>), see the Makefile.
    .section .rodata
    .global _binary_font_8x16_start
    .global _binary_font_8x16_end
    .global _binary_font_16x32_start
    .global _binary_font_16x32_end
    .balign 16
_binary_font_8x16_start:
    .incbin "kernel/boot/font/terminus-8x16.psf"
_binary_font_8x16_end:
    .balign 16
_binary_font_16x32_start:
    .incbin "kernel/boot/font/terminus-16x32.psf"
_binary_font_16x32_end:
    .global _binary_fonts_aa_start
    .global _binary_fonts_aa_end
    .balign 16
_binary_fonts_aa_start:
    .incbin "kernel/gfx/fonts.bin"
_binary_fonts_aa_end:
    .section .note.GNU-stack,"",@progbits
