/* Kernel linker script: higher-half image at 0xFFFFFFFF80000000 laid out for Limine.
   Each output section gets its own PT_LOAD so the bootloader can map them with
   distinct permissions (text RX, rodata R, data RW). */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_main)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    . = 0xFFFFFFFF80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    __data_end = .;

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
