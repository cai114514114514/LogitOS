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

    ; --- enable SSE/SSE2 + the x87 FPU (M15) -------------------------------
    ; The x86-64 SysV ABI passes float/double in XMM, so any C floating point
    ; (the upcoming JS engine) needs SSE on. Clear CR0.EM (no FPU emulation),
    ; set CR0.MP, then CR4.OSFXSR (FXSAVE/FXRSTOR + SSE) and CR4.OSXMMEXCPT
    ; (deliver #XM for SIMD faults). fninit gives a clean x87 state; a default
    ; MXCSR (0x1F80) masks all SIMD exceptions. We do NOT use lazy FP (CR0.TS).
    mov rax, cr0
    and rax, ~(1 << 2)         ; CR0.EM = 0
    or  rax, (1 << 1)          ; CR0.MP = 1
    or  rax, (1 << 5)          ; CR0.NE = 1 (native x87 exceptions)
    mov cr0, rax

    mov rax, cr4
    or  rax, (1 << 9)          ; CR4.OSFXSR
    or  rax, (1 << 10)         ; CR4.OSXMMEXCPT
    mov cr4, rax

    fninit
    sub rsp, 8
    mov dword [rsp], 0x1F80
    ldmxcsr [rsp]
    add rsp, 8
    ; ----------------------------------------------------------------------

    mov edi, edi            ; zero-extend Multiboot2 info ptr into rdi (1st arg)
    call kernel_main

    ; kernel_main should never return; halt forever if it does.
.hang:
    hlt
    jmp .hang
