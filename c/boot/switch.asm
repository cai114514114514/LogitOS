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

    ; Return with interrupts DISABLED, always. popfq below restores the INCOMING
    ; thread's saved RFLAGS, and a FIRST-RUN thread's hand-built frame has IF=1 --
    ; so clear IF in the saved flags BEFORE popfq (closing the one-instruction
    ; interrupt window between popfq and a trailing cli): without it the switch
    ; leaves IF=1 while the incoming side still holds
    ; g_sched_lock (schedule's [context_switch .. spin_unlock(g_sched_lock)] window,
    ; and the bootstraps before they release it). A timer IRQ landing there enters
    ; interrupt_handler and acquires g_bkl WHILE holding g_sched_lock -- reverse
    ; lock order -> ABBA deadlock vs a core holding g_bkl that waits for g_sched_lock
    ; in schedule() (the -smp 4 "all cores wedged in spin_lock" freeze). Every resume
    ; site re-enables IF at its own controlled point (schedule's `if(flags&IF) sti`,
    ; kthread_bootstrap's sti, the iretq into ring 3), so forcing IF=0 here is safe.
    and qword [rsp], ~0x200   ; force IF=0 in the restored RFLAGS
    popfq
    cli                       ; belt-and-braces (IF already 0 via the mask above)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
