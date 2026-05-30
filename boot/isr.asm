; ============================================================================
; Aqua OS - interrupt service routine stubs (64-bit)
;
; One stub per vector 0..47. Each pushes a (dummy where needed) error code and
; the vector number, then jumps to a common path that saves all GP registers,
; calls the C dispatcher, restores, and `iretq`s.
;
; Exceptions 8, 10-14 and 17 push a real error code; everything else gets a
; dummy 0 so the stack layout is uniform.
; ============================================================================

extern interrupt_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0          ; dummy error code
    push %1         ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push %1         ; vector number (CPU already pushed the error code)
    jmp isr_common
%endmacro

section .text
bits 64

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
; IRQs 0..15 -> vectors 32..47
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; struct registers* -> first argument
    cld
    call interrupt_handler

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

    add rsp, 16         ; discard vector + error code
    iretq

; Table of stub addresses, indexed by vector, for the C side to install.
section .rodata
global isr_stub_table
isr_stub_table:
%assign vec 0
%rep 48
    dq isr %+ vec
%assign vec vec+1
%endrep
