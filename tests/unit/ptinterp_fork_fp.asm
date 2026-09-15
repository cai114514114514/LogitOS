; SPDX-License-Identifier: MIT
; Seed all 16 XMM registers, x87 and MXCSR before the actual inline fork ABI.
; Both parent and child compare the saved state, then restore the C caller's
; original FP state. The kernel cannot pass by merely clearing fresh state.
bits 64
global ptinterp_fork_fp
section .text
ptinterp_fork_fp:
    push rbp
    mov rbp, rsp
    sub rsp, 1552
    and rsp, -16
    fxsave [rsp]
    fninit
    fld1
    fld1
    ldmxcsr [rel rounding]
%assign reg 0
%rep 16
    movdqu xmm %+ reg, [rel seed + reg * 16]
%assign reg reg + 1
%endrep
    fxsave [rsp + 512]
    mov eax, SYS_FORK
    int 0x80
    mov r10, rax
    fxsave [rsp + 1024]
    mov dword [rdi], 0
    mov eax, [rsp + 512 + 24]
    cmp eax, [rsp + 1024 + 24]
    jne .done
    mov eax, [rsp + 512]
    cmp eax, [rsp + 1024]
    jne .done
    mov ecx, 160
.xmm:
    mov rax, [rsp + 512 + rcx]
    cmp rax, [rsp + 1024 + rcx]
    jne .done
    add ecx, 8
    cmp ecx, 416
    jb .xmm
    faddp st1, st0
    fstp qword [rsp + 1536]
    mov rax, 0x4000000000000000 ; 2.0, exact in every rounding mode
    cmp [rsp + 1536], rax
    jne .done
    mov dword [rdi], 1
.done:
    fxrstor [rsp]
    mov rax, r10
    mov rsp, rbp
    pop rbp
    ret
section .rodata
align 16
rounding: dd 0x3f80
align 16
seed:
%assign reg 0
%rep 16
    dq 0x1020304050607000 + reg, 0xf0e0d0c0b0a09000 + reg
%assign reg reg + 1
%endrep
