; Logit OS CLI runtime: the kernel's execve() lays out a SysV stack
;   [argc][argv0..argvN][NULL][envp..][NULL][auxv...]
; with rsp pointing at argc (16-byte aligned). Read argc/argv, publish envp,
; call main, exit.
;
; WHY __libc_environ_hook IS DEFINED HERE AND NOT IN THE LIBC.
; It used to live in c/apps/libc/src/stdlib.c, which meant nothing ever set it
; and getenv() found an empty environment in every program on this machine --
; the kernel builds a complete envp (c/kernel/exec/exec.c) and crt0 threw the
; pointer away. The obvious fix, `extern __libc_environ_hook` here, does not
; work: this crt0 is linked by ~30 coreutils through CLI_RULE, and those
; programs do NOT link mini-libc at all (they use c/apps/clib.h inline
; syscalls). An extern would be an undefined symbol in every one of them.
;
; So the STORAGE moves here and the libc takes an extern. A program that links
; the libc gets a populated environment; a program that does not still links.
global _start
global __libc_environ_hook
global environ
extern main

section .bss
align 8
__libc_environ_hook: resq 1
environ:             resq 1

section .text
bits 64
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv (char **)
    lea rax, [rsi + rdi*8 + 8]      ; &argv[argc + 1] == envp; the same address
    mov [rel __libc_environ_hook], rax  ; c/apps/coreutils/sh.c reaches via rt_envp
    mov [rel environ], rax  ; and POSIX's `environ` itself, valid before main
    and rsp, -16            ; keep the 16-byte alignment ABI wants before a call
    call main
    mov edi, eax            ; exit status = main()'s return value
    mov eax, 2              ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang
