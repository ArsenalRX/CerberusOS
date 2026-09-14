// Embeds the generated symbol table blob. SYMS_FILE is passed on the command
// line by the Makefile: an empty table for the first link pass, the real one
// for the second. The section is placed last in the image so its size cannot
// move any other symbol between the two passes.
    .section .ksymtab,"a"
    .balign 16
    .incbin SYMS_FILE
    .section .note.GNU-stack,"",@progbits
