/* Game image: flat, non-relocatable, in the low 4 GiB so that every pointer fits in 32 bits. */
ENTRY(ss_guest_entry)
SECTIONS
{
    . = 0x0A000000;     /* inside the host's arena, after the GBA address space */
    __image_start = .;
    .text : { *(.text .text.*) }
    __text_end = .;
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    . = ALIGN(0x1000);
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
