; SPDX-License-Identifier: MIT
; v3 keeps each application's original CRT. Dispatch authenticated worker
; activation first, then restore the exact original stack for legacy main.
; In particular, typing --agent never changes the kernel's activation mode.
bits 64
global _agent_start
extern _start, ag_activation, __libc_environ_hook, environ
section .text
_agent_start:
    mov r12, rsp
    mov rdi, [rsp]
    lea rsi, [rsp+8]
    lea rax, [rsi+rdi*8+8]
    mov [rel __libc_environ_hook], rax
    mov [rel environ], rax
    and rsp, -16
    call ag_activation
    cmp eax, -1
    jne .exit
    mov rsp, r12
    jmp _start
.exit:
    mov edi, eax
    mov eax, 2
    int 0x80
    jmp .exit
