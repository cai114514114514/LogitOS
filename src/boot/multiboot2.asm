; ============================================================================
; Aqua OS - Multiboot2 header
;
; GRUB scans the first 32 KiB of the kernel image for this 8-byte-aligned
; structure. The linker script places this section first (at 1 MiB), so the
; header lands well within that window.
; ============================================================================

MB2_MAGIC      equ 0xe85250d6      ; multiboot2 magic
MB2_ARCH       equ 0               ; 0 = i386 32-bit protected mode

section .multiboot_header
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd header_end - header_start                                   ; header length
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (header_end - header_start)) ; checksum

    ; --- framebuffer request tag: ask GRUB for a 1280x800x32 linear mode ---
    align 8
    dw 5        ; type = framebuffer
    dw 0        ; flags
    dd 20       ; size
    dd 1280     ; width
    dd 800      ; height
    dd 32       ; depth (bits per pixel)

    ; --- required end tag ---
    align 8
    dw 0    ; type = 0
    dw 0    ; flags
    dd 8    ; size
header_end:
