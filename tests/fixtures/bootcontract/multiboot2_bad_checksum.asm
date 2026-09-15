; ============================================================================
; Logit OS - Multiboot2 header
;
; Negative-control copy of c/boot/multiboot2.asm.  Only the checksum differs:
; it is deliberately one too large so the source gate must reject this file.
; ============================================================================

MB2_MAGIC      equ 0xe85250d6      ; multiboot2 magic
MB2_ARCH       equ 0               ; 0 = i386 32-bit protected mode

section .multiboot_header
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd header_end - header_start                                   ; header length
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (header_end - header_start)) + 1 ; BAD checksum

    ; --- framebuffer request tag: ask for a 1280x800x32 linear mode ---
    align 8
    dw 5        ; type = framebuffer
    dw 1        ; flags = optional
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
