; ============================================================================
; The first ten instructions of every thread this process creates.
;
; WHY THERE IS ASSEMBLY HERE AT ALL. A new ring-3 thread is entered by the
; kernel through c/boot/enter_user.asm's IRETQ, and the last thing that code
; does before the IRETQ is `mov fs, ax` -- which ZEROES the %fs base. So the
; kernel cannot hand a thread its thread-local-storage pointer in a register
; (the register state at the IRETQ is not the thread line's to choose) and
; cannot pre-load the MSR either (that instruction destroys it). Editing shared
; boot assembly to make room would be a change to another line's file for a
; benefit only userland sees.
;
; What DOES survive is the stack. So SYS_THREAD_CREATE writes two words at the
; top of the new thread's stack and this stub reads them:
;
;       [rsp]   = arg           the void* handed to pthread_create
;       [rsp+8] = tls           the thread pointer (%fs base) to install
;
; and its very first act is the syscall that installs the second one. Until
; that returns, this thread has no `__thread` variables, no pthread_self() and
; no errno -- so nothing above this line may run before it, which is why the
; C body is a separate function and this stub calls nothing.
;
; The number 112 is SYS_SET_TLS. nasm cannot include the C ABI header, so it is
; written out here and ASSERTED against the header in pthread.c
; (_Static_assert on SYS_SET_TLS) -- the same shape crt0_cli.asm uses for
; SYS_EXIT, and the assert is what stops the two drifting.
; ============================================================================

global __logit_thread_entry
extern __logit_thread_body

section .text
bits 64

__logit_thread_entry:
    mov  rdi, [rsp + 8]         ; the thread pointer
    mov  rax, 112               ; SYS_SET_TLS  (asserted in pthread.c)
    int  0x80

    mov  rdi, [rsp]             ; arg -- passed on for symmetry; the body also
                                ; finds it in the TCB, which is what it uses
    ; SysV wants RSP 16-byte aligned at a `call` site, i.e. the callee sees
    ; RSP == 8 (mod 16) after the return address is pushed. This frame was
    ; entered by IRETQ, not by a call, so nothing established that -- align it
    ; here. This is the same trap thread_create()'s long comment in
    ; c/kernel/sched/sched.c describes, and the same exemption: no C prologue
    ; runs before this instruction, so there is nothing to have crashed yet.
    and  rsp, -16
    call __logit_thread_body

    ; __logit_thread_body ends in SYS_THREAD_EXIT and never returns. If it ever
    ; did, falling off the end of a thread stack would be a wild jump, so stop
    ; here instead -- visible as a hung thread rather than as memory corruption
    ; somewhere else.
.hang:
    hlt                         ; #GP in ring 3: dies loudly, does not spin a core
    jmp .hang
