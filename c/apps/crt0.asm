; Logit OS userland C runtime: call app_main, then exit.
;
; __libc_environ_hook is DEFINED here and left NULL on purpose. A GUI app is
; not started by execve -- wm_launch spawns it with a bare stack and no argv or
; envp, and it takes its argument through SYS_GET_ARG instead. But the browser
; links this crt0 AND mini-libc, and the libc now takes the symbol as an
; extern (the storage moved into crt0 so that the ~30 coreutils which link
; crt0_cli.asm WITHOUT the libc still link). Without this definition the
; browser fails at link time. NULL is the honest value: env_init() in
; c/apps/libc/src/stdlib.c starts an empty environment when the hook is unset,
; which is exactly what a GUI app has.
global _start
global __libc_environ_hook
global environ
extern app_main

section .bss
align 8
__libc_environ_hook: resq 1
environ:             resq 1

section .text
bits 64
_start:
    call app_main
    mov eax, 2          ; SYS_EXIT
    xor edi, edi
    int 0x80
.hang:
    jmp .hang
