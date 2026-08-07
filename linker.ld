/* Logit OS kernel linker script.
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

        /* Device-model driver registry. DRIVER_DECLARE (c/drivers/core/driver.h)
         * emits one `struct driver *` here per driver, so adding a driver is
         * adding a file -- there is no central list to edit. Bracketed
         * explicitly rather than relying on lld's __start_/__stop_ orphan
         * placement, which could put the section past _kernel_end where the
         * PMM would hand the frames out as free memory. */
        . = ALIGN(8);
        __start_logit_drivers = .;
        KEEP(*(logit_drivers))
        __stop_logit_drivers = .;
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
