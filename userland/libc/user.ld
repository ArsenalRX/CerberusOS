/* Link layout for Lumen user programs: position-independent, linked at
   address 0, three segments (code r-x, constants r--, data rw-) so nothing is
   ever writable and executable. The relocation table is bracketed so the
   startup code can apply it itself (there is no dynamic linker yet). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* The image starts at address 0 (the ELF headers are part of the first
       segment): a linker marks an image that starts anywhere else as
       fixed-address, even when asked for a position-independent one. */
    . = SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .rela.dyn : {
        __rela_start = .;
        *(.rela .rela.*)
        __rela_end = .;
    } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata

    . = ALIGN(0x1000);
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic : { *(.dynamic) } :data
    .got : { *(.got .got.plt) } :data
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) *(.interp) }
}
