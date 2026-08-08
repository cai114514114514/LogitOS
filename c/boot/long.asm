; ============================================================================
; Logit OS - 64-bit long mode entry
;
; Reached via the far jump in boot.asm once paging + long mode are live.
; We zero the data segment selectors (segmentation is flat in long mode) and
; hand off to the C kernel.
; ============================================================================

global long_mode_start
extern kernel_main
extern cpu_early_init

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

    ; --- what we deliberately do NOT enable: AVX ---------------------------
    ; Enabling AVX means CR4.OSXSAVE plus XSETBV setting XCR0 bits 1 (SSE) and
    ; 2 (AVX). Two lines. The reason they are not here:
    ;
    ;   boot/isr.asm wraps every C interrupt handler in FXSAVE/FXRSTOR.
    ;   FXSAVE stores 512 bytes: x87 state and XMM0-15. It does NOT store the
    ;   upper 128 bits of the YMM registers. Turn AVX on while that is still
    ;   the save path and every interrupt silently truncates YMM in whatever
    ;   it interrupted -- data corruption that only appears when a trap lands
    ;   inside a vectorised routine, i.e. rarely and non-deterministically.
    ;
    ; So AVX requires migrating isr_common to XSAVE/XRSTOR first: a save area
    ; sized from CPUID.0x0D:0 ECX (not the compile-time 512), 64-byte aligned,
    ; zeroed XSAVE header, EDX:EAX = the state mask on both XSAVE and XRSTOR,
    ; and the same treatment anywhere else FP state is stashed. That is a
    ; change to the fault path, so it is its own piece of work.
    ;
    ; AES-NI and PCLMULQDQ (c/crypto/aead/aes_ni.c) need none of this: they
    ; touch XMM0-15 only, which FXSAVE already covers. They also need no CR4
    ; bit at all -- CPUID reports them and they are simply usable, which is why
    ; nothing about them appears above.
    ;
    ; CPUID.0x0D is read and printed at boot anyway (c/crypto/cpu_report.c), so
    ; the XSAVE area size for this machine is on the serial log for whoever
    ; does the migration.
    ; ----------------------------------------------------------------------

    ; --- the ring-3 protection bits (EFER.NXE, CR4.SMEP) -------------------
    ; Set HERE, in the same place as the other CPU-mode bits, and not from C,
    ; for one reason: EFER.NXE must be on before ANY page-table entry carries
    ; bit 63. With NXE clear, bit 63 is a RESERVED bit, and a PTE that sets it
    ; faults with PF_RSVD -- which mm_fault_classify() correctly refuses to
    ; handle, so the process dies with no diagnosis. Enabling the bit before
    ; the first user page can exist makes that ordering unrepresentable rather
    ; than merely observed to be true.
    ;
    ; Both are CPUID-gated. A CPU without NX (or QEMU's default `qemu64` model
    ; with the feature masked off) must not have its EFER poked at a reserved
    ; bit, and elf.c asks cpu_prot_nx() -- which reads the SAME CPUID bit --
    ; before it sets bit 63 on anything. The two answers cannot disagree.
    ;
    ; CPUID clobbers eax/ebx/ecx/edx and leaves edi (the Multiboot2 info
    ; pointer, still live for the kernel_main call below) untouched.

    ; NX: CPUID.0x80000001:EDX bit 20 -> EFER.NXE (bit 11).
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001         ; is the extended leaf implemented at all?
    jb .no_nx
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 20           ; NX
    jz .no_nx
    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or  eax, 1 << 11            ; EFER.NXE
    wrmsr
.no_nx:

    ; SMEP: CPUID.7:EBX bit 7 -> CR4.SMEP (bit 20). Stops ring 0 executing any
    ; page whose effective U/S is user. The kernel never does that on purpose,
    ; so this bit costs nothing and turns "the kernel jumped into a user buffer"
    ; from a silent full compromise into a fault.
    ;
    ; SMAP (CR4 bit 21) is deliberately NOT set here. It would fault the kernel
    ; on every DIRECT read/write of a user pointer, and this kernel does that in
    ; ~70 places across files this line does not own. See c/kernel/cpu/prot.c
    ; for the audit and the reason it stays off.
    xor eax, eax
    cpuid
    cmp eax, 7                  ; is leaf 7 implemented?
    jb .no_smep
    mov eax, 7
    xor ecx, ecx
    cpuid
    test ebx, 1 << 7            ; SMEP
    jz .no_smep
    mov rax, cr4
    or  rax, (1 << 20)          ; CR4.SMEP
    mov cr4, rax
.no_smep:
    ; ----------------------------------------------------------------------

    ; Decode CPUID once, before anything can race, and pick the crypto
    ; backends. Pure CPUID + a pointer store: no serial, heap, IDT or timer
    ; needed, which is why it can run this early. Reporting happens later,
    ; once kprintf exists.
    ; The Multiboot2 info pointer is parked in ebx across the call rather than
    ; pushed: rbx is callee-saved, and pushing would flip rsp's 16-byte parity
    ; for the callee (SysV wants rsp%16==8 on entry, which is what plain
    ; `call` from the untouched boot stack gives).
    mov ebx, edi
    call cpu_early_init
    mov edi, ebx
    ; ----------------------------------------------------------------------

    mov edi, edi            ; zero-extend Multiboot2 info ptr into rdi (1st arg)
    call kernel_main

    ; kernel_main should never return; halt forever if it does.
.hang:
    hlt
    jmp .hang
