; ============================================================================
; Logit OS - 32-bit boot entry
;
; GRUB hands control here in 32-bit protected mode (per Multiboot2). We:
;   1. set up a stack
;   2. verify we were loaded by a Multiboot2 loader
;   3. verify the CPU supports CPUID and long mode
;   4. build temporary identity-mapped page tables (first 1 GiB, 2 MiB pages)
;   5. enable PAE, set EFER.LME, enable paging
;   6. load a 64-bit GDT and far-jump into 64-bit code (long.asm)
; ============================================================================

global start
extern long_mode_start

section .text
bits 32
start:
    mov esp, stack_top          ; establish a stack (eax still holds MB2 magic)
    mov edi, ebx                ; stash Multiboot2 info pointer (survives to long mode)

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start  ; far jump -> 64-bit long mode

; ----------------------------------------------------------------------------
; Sanity checks. On failure we print "ERR: <code>" and halt.
;   '0' = not loaded by a Multiboot2 loader
;   '1' = CPUID not supported
;   '2' = long mode not supported
; ----------------------------------------------------------------------------
check_multiboot:
    cmp eax, 0x36d76289         ; Multiboot2 bootloader magic
    jne .fail
    ret
.fail:
    mov al, '0'
    jmp error

check_cpuid:
    ; CPUID is supported if we can flip bit 21 (ID) in EFLAGS.
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx                    ; restore original EFLAGS
    popfd
    cmp eax, ecx
    je .fail
    ret
.fail:
    mov al, '1'
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001         ; is extended function 0x80000001 available?
    jb .fail
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29           ; LM bit
    jz .fail
    ret
.fail:
    mov al, '2'
    jmp error

; ----------------------------------------------------------------------------
; Build 4-level page tables. Identity-map the first 1 GiB using 2 MiB pages:
;   PML4[0] -> PDPT
;   PDPT[0] -> PD
;   PD[0..511] -> 0x000000 .. 0x40000000  (huge pages)
; ----------------------------------------------------------------------------
setup_page_tables:
    mov eax, pdpt_table
    or eax, 0b11                ; present | writable
    mov [pml4_table], eax

    mov eax, pd_table
    or eax, 0b11
    mov [pdpt_table], eax

    mov ecx, 0
.map_pd:
    mov eax, 0x200000           ; 2 MiB
    mul ecx                     ; eax = ecx * 2 MiB
    or eax, 0b10000011          ; present | writable | huge
    mov [pd_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd
    ret

enable_paging:
    mov eax, pml4_table
    mov cr3, eax                ; load top-level table

    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, 1 << 8              ; EFER.LME (long mode enable)
    wrmsr

    mov eax, cr0
    or eax, 1 << 31             ; CR0.PG (paging)
    mov cr0, eax
    ret

; al = single-character error code
error:
    mov dword [0xb8000], 0x4f524f45   ; "ER"
    mov dword [0xb8004], 0x4f3a4f52   ; "R:"
    mov word  [0xb8008], 0x4f20       ; ' '
    mov byte  [0xb800a], al           ; code char
    mov byte  [0xb800b], 0x4f         ; white on red
.hang:
    hlt
    jmp .hang

; ----------------------------------------------------------------------------
; 64-bit GDT: a null descriptor plus a single flat 64-bit code segment.
; ----------------------------------------------------------------------------
section .rodata
gdt64:
    dq 0                                                    ; null descriptor
.code: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)        ; exec | type | present | 64-bit
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; ----------------------------------------------------------------------------
; Page tables + kernel stack. GRUB zero-fills .bss for Multiboot ELF images,
; so unset page-table entries are guaranteed to be 0 (not present).
; ----------------------------------------------------------------------------
section .bss
align 4096
pml4_table:  resb 4096
pdpt_table:  resb 4096
pd_table:    resb 4096
stack_bottom:
    resb 32768                  ; 32 KiB boot stack (TLS/RSA path is stack-heavy)
stack_top:
