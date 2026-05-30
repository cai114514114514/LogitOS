/* Aqua OS userland link script.
 *
 * Linked at 1 GiB (0x40000000): above the kernel's identity-mapped first 1 GiB
 * (which uses 2 MiB huge pages), so the VMM can map fresh 4 KiB user pages
 * there without colliding with a huge-page mapping. Everything goes into one
 * RWX PT_LOAD segment so the ELF loader maps a single contiguous region. */

ENTRY(_start)

PHDRS
{
    load PT_LOAD FLAGS(7);   /* RWX */
}

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text .text.*) }     :load
    .rodata : { *(.rodata .rodata.*) } :load
    .data   : { *(.data .data.*) }     :load
    .bss    : { *(.bss .bss.*) *(COMMON) } :load

    /DISCARD/ : { *(.eh_frame) *(.note .note.*) *(.comment) }
}
