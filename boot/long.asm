; ============================================================================
; Aqua OS - 64-bit long mode entry
;
; Reached via the far jump in boot.asm once paging + long mode are live.
; We zero the data segment selectors (segmentation is flat in long mode) and
; hand off to the C kernel.
; ============================================================================

global long_mode_start
extern kernel_main

section .text
bits 64
long_mode_start:
    ; In long mode segment bases are ignored; null data selectors are fine.
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov edi, edi            ; zero-extend Multiboot2 info ptr into rdi (1st arg)
    call kernel_main

    ; kernel_main should never return; halt forever if it does.
.hang:
    hlt
    jmp .hang
