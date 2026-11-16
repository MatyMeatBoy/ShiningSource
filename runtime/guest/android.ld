/* Relocatable x32 Android image. Separate 16 KiB pages for code and data. */
ENTRY(ss_guest_entry)
PHDRS
{
    ro PT_LOAD FLAGS(4);
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS
{
    . = 0x0A000000;
    __image_start = .;
    .rodata : { *(.rodata .rodata.*) } :ro
    .dynsym : { *(.dynsym) } :ro
    .gnu.hash : { *(.gnu.hash) } :ro
    .hash : { *(.hash) } :ro
    .dynstr : { *(.dynstr) } :ro
    .rela.dyn : { *(.rela.dyn) } :ro
    . = ALIGN(0x4000);
    .text : { *(.text .text.*) } :code
    __text_end = .;
    . = ALIGN(0x4000);
    .data : { *(.data .data.*) *(.got .got.*) } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .bss : { *(.bss .bss.* COMMON) } :data
    . = ALIGN(0x4000);
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
