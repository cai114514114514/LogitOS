; Aqua OS - userland entry stub. Calls umain, then sys_exit with its result.
global _start
extern umain

section .text
bits 64
_start:
    call umain
    mov edi, eax        ; exit code = umain() return value
    mov eax, 2          ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang
