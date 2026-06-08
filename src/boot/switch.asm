; ============================================================================
; Aether OS - context switch
;
;   void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
;     rdi = where to save the outgoing thread's stack pointer
;     rsi = the incoming thread's saved stack pointer
;
; Saves the callee-saved registers + RFLAGS of the current thread, swaps the
; stack pointer, and restores the next thread's state. A freshly created thread
; has a hand-built stack so the final `ret` lands on its entry function.
; ============================================================================

global context_switch
section .text
bits 64

context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq

    mov [rdi], rsp      ; save outgoing stack pointer
    mov rsp, rsi        ; load incoming stack pointer

    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
