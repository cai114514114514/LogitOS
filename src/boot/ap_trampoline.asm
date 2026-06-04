; ============================================================================
; AP startup trampoline. A STARTUP IPI begins an application processor in 16-bit
; real mode at (vector << 12). We copy this blob to physical 0x8000 (vector 8)
; and SIPI with vec=8, so every absolute reference below is hardcoded off 0x8000.
;
; Flow: real mode -> protected mode (PAE) -> load the kernel's CR3 -> long mode
; -> set up SSE -> load this AP's stack -> call ap_entry(). The BSP fills the
; arg block at 0x8F00 (cr3, stack, entry) before each SIPI.
; ============================================================================

TRAMP equ 0x8000
ARGS  equ 0x8F00          ; cr3 @ +0, stack_top @ +8, entry @ +16

global ap_tramp_start
global ap_tramp_end

section .text
ap_tramp_start:

bits 16
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [TRAMP + (gdt32_ptr - ap_tramp_start)]
    mov eax, cr0
    or  al, 1                              ; CR0.PE
    mov cr0, eax
    jmp 0x08:(TRAMP + (pm32 - ap_tramp_start))

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or  eax, 1 << 5                        ; CR4.PAE
    mov cr4, eax
    mov eax, [ARGS]                        ; kernel CR3 (PML4 phys, < 4 GiB)
    mov cr3, eax
    mov ecx, 0xC0000080                    ; EFER
    rdmsr
    or  eax, 1 << 8                        ; LME
    wrmsr
    mov eax, cr0
    or  eax, 1 << 31                       ; CR0.PG
    or  eax, 1 << 0                         ; keep PE
    mov cr0, eax
    lgdt [TRAMP + (gdt64_ptr - ap_tramp_start)]
    jmp 0x08:(TRAMP + (lm64 - ap_tramp_start))

bits 64
lm64:
    mov ax, 0x10
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; SSE/SSE2 + x87, same as the BSP (long.asm) so ring-3 FP works on this CPU
    mov rax, cr0
    and rax, ~(1 << 2)
    or  rax, (1 << 1)
    or  rax, (1 << 5)
    mov cr0, rax
    mov rax, cr4
    or  rax, (1 << 9)
    or  rax, (1 << 10)
    mov cr4, rax
    fninit
    mov rsp, [ARGS + 8]                    ; this AP's stack
    sub rsp, 8
    mov dword [rsp], 0x1F80
    ldmxcsr [rsp]
    add rsp, 8
    mov rax, [ARGS + 16]                   ; ap_entry
    xor rbp, rbp
    call rax
.hang:
    hlt
    jmp .hang

; --- GDTs (referenced absolutely off TRAMP) ---
align 8
gdt32:
    dq 0
    dq 0x00CF9A000000FFFF                  ; 32-bit code
    dq 0x00CF92000000FFFF                  ; 32-bit data
gdt32_ptr:
    dw 23
    dd TRAMP + (gdt32 - ap_tramp_start)

align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF                  ; 64-bit code (L=1)
    dq 0x00CF92000000FFFF                  ; data
gdt64_ptr:
    dw 23
    dd TRAMP + (gdt64 - ap_tramp_start)

ap_tramp_end:
