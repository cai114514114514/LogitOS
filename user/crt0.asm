; Aqua OS userland C runtime: call app_main, then exit.
global _start
extern app_main

section .text
bits 64
_start:
    call app_main
    mov eax, 2          ; SYS_EXIT
    xor edi, edi
    int 0x80
.hang:
    jmp .hang
