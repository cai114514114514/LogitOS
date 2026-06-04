; ============================================================================
; Aqua OS - drop to ring 3
;
;   void enter_user(uint64_t entry, uint64_t user_rsp);
;     rdi = user entry point, rsi = user stack top
;
; Builds an interrupt-return frame for ring 3 and `iretq`s into it.
; ============================================================================

global enter_user
global ring3_bootstrap
global fork_ret

section .text
bits 64

; First entry of a ring-3 thread: the scheduler's context_switch "returns" here
; with r15 = entry point and r14 = user stack (set up by thread_create_user).
ring3_bootstrap:
    mov rdi, r15
    mov rsi, r14
    call enter_user
.hang:
    jmp .hang

; First entry of a forked child: context_switch "returns" here with rsp pointing
; at a copy of the parent's `struct registers` frame (rax already 0). Restore the
; GPRs and iretq into ring 3 -- mirrors the tail of isr_common in boot/isr.asm.
; FP/SSE state is not restored here: SysV XMM regs are caller-saved, so the
; compiler assumes nothing survives across the fork() call in the child.
fork_ret:
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

    push 0x23           ; SS
    push rsi            ; RSP
    push 0x202          ; RFLAGS (IF set)
    push 0x1B           ; CS (user code, RPL 3)
    push rdi            ; RIP
    iretq
