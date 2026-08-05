; ============================================================================
; Logit OS - load the GDT and TSS
;
;   void gdt_flush(void *gdt_ptr);   rdi = &gdtr
;   void tss_flush(uint16_t sel);    di  = TSS selector
;
; CS keeps selector 0x08 (the new GDT's kernel-code descriptor is identical to
; the boot GDT's), so only the data selectors need reloading.
; ============================================================================

global gdt_flush
global tss_flush

section .text
bits 64

gdt_flush:
    lgdt [rdi]
    mov ax, 0x10        ; kernel data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    ret

tss_flush:
    mov ax, di
    ltr ax
    ret
