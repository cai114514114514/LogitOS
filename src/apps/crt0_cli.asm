; Aqua OS CLI runtime: the kernel's execve() lays out a SysV stack
;   [argc][argv0..argvN][NULL][envp..][NULL]
; with rsp pointing at argc (16-byte aligned). Read argc/argv, call main, exit.
global _start
extern main

section .text
bits 64
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv (char **)
    and rsp, -16            ; keep the 16-byte alignment ABI wants before a call
    call main
    mov edi, eax            ; exit status = main()'s return value
    mov eax, 2              ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang
