; SPDX-License-Identifier: MIT
bits 64
global _start
extern ptinterp_boot
section .text
_start:
    mov r12, rsp
    mov rdi, rsp
    and rsp, -16
    call ptinterp_boot
    mov rsp, r12
    xor edx, edx
    jmp rax
