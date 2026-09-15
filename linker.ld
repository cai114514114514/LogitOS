/* Logit OS kernel linker script.
 *
 * The kernel is loaded at 32 MiB.  The old conventional 1 MiB address crosses
 * firmware-owned low-memory ranges on real UEFI machines (and OVMF puts ACPI
 * NVS in the old image span). 32 MiB remains inside the native protocol's
 * first-1-GiB identity runway while leaving that firmware workspace alone.
 * The .boot placement below remains solely for the one-release GRUB escape
 * hatch and test-only oracle; shipping kernels contribute no scanned header.
 */

ENTRY(logit_native_start)

SECTIONS
{
    . = 32M;
    _kernel_start = .;      /* first physical byte occupied by the kernel */

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
