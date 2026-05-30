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
