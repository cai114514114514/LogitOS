/* Aether OS kernel linker script.
 *
 * The kernel is loaded at the conventional 1 MiB mark. The Multiboot2 header
 * is forced to the very front so GRUB finds it within the first 32 KiB.
 */

ENTRY(start)

SECTIONS
{
    . = 1M;

    .boot ALIGN(8) :
    {
        KEEP(*(.multiboot_header))
    }

    .text ALIGN(4K) :
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data .data.*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4K);
    _kernel_end = .;        /* first free physical byte after the kernel image */

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
