; AquaScript / mini-libc setjmp + longjmp (x86-64 SysV).
; jmp_buf layout (8 longs): [0]rbx [1]rbp [2]r12 [3]r13 [4]r14 [5]r15 [6]rsp [7]rip
global setjmp
global longjmp

section .text
bits 64

setjmp:                 ; int setjmp(jmp_buf env)  -- rdi = env
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]      ; caller's rsp (undo the call's push)
    mov [rdi + 48], rax
    mov rax, [rsp]          ; return address
    mov [rdi + 56], rax
    xor eax, eax            ; direct call returns 0
    ret

longjmp:                ; void longjmp(jmp_buf env, int val)  -- rdi=env, rsi=val
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov eax, esi
    test eax, eax
    jnz .nz
    mov eax, 1             ; longjmp(env, 0) appears as setjmp returning 1
.nz:
    jmp [rdi + 56]         ; resume at the saved return address
