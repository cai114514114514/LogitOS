; ============================================================================
; Logit OS - drop to ring 3
;
;   void enter_user(uint64_t entry, uint64_t user_rsp);
;     rdi = user entry point, rsi = user stack top
;
; Builds an interrupt-return frame for ring 3 and `iretq`s into it.
; ============================================================================

global enter_user
global ring3_bootstrap
global fork_ret
extern sched_unlock_new_thread     ; sched.c: drops g_sched_lock + the BKL on first run

section .text
bits 64

; First entry of a ring-3 thread: the scheduler's context_switch "returns" here
; with r15 = entry point and r14 = user stack (set up by thread_create_user).
; The thread arrives holding g_sched_lock + the BKL (handed off through the
; context switch); release BOTH before iretq. r15/r14 are callee-saved across the
; SysV call so they survive; IF is set by enter_user's iretq frame (RFLAGS=0x202).
ring3_bootstrap:
    call sched_unlock_new_thread
    mov rdi, r15
    mov rsi, r14
    call enter_user
.hang:
    jmp .hang

; First entry of a forked child: context_switch "returns" here with rsp pointing
; at a copy of the parent's `struct registers` frame (rax already 0). Restore the
; GPRs and iretq into ring 3 -- mirrors the tail of isr_common in boot/isr.asm.
; FP/SSE state is reset to a clean default below (fninit + default MXCSR):
; without that the child would start with whatever XMM/MXCSR bits the CPU
; happened to hold (leftovers from other processes' kernel paths -- a
; cross-process info leak). Full fenv inheritance would need thread_fork to copy
; the parent's FXSAVE area; SysV XMM regs are caller-saved, so the compiler
; assumes nothing survives the fork() call in the child regardless.
; The child arrives holding g_sched_lock + the BKL; release both first. A `call`
; is rsp-net-zero (push+pop a return addr), so the regs frame below is intact and
; the subsequent `pop r15` is still correct; the child's IF comes from cr->rflags
; via iretq. sched_unlock_new_thread clobbers no callee-saved regs the frame needs.
fork_ret:
    call sched_unlock_new_thread
    fninit
    sub rsp, 8
    mov dword [rsp], 0x1F80  ; default MXCSR
    ldmxcsr [rsp]
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16         ; discard vector + error_code
    iretq

enter_user:
    mov ax, 0x23        ; user data selector (RPL 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Clean default FP/SSE state for the fresh process: without this the new
    ; ring-3 thread starts with whatever XMM0-15/MXCSR the previous process or
    ; the kernel left on this core (a cross-process information leak).
    fninit
    sub rsp, 8
    mov dword [rsp], 0x1F80  ; default MXCSR
    ldmxcsr [rsp]
    add rsp, 8

    push 0x23           ; SS
    push rsi            ; RSP
    push 0x202          ; RFLAGS (IF set)
    push 0x1B           ; CS (user code, RPL 3)
    push rdi            ; RIP
    iretq
