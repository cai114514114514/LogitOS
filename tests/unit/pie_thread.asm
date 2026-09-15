; Same IRET entry convention as pthread_entry.asm, without linking a full libc.
global pie_thread_start
extern pie_worker
section .text
bits 64
pie_thread_start:
    ; SYS_THREAD_CREATE supplies arg at [rsp], TLS at [rsp+8]; mirror the
    ; production pthread_entry.asm ABI before executing any C TLS access.
    mov rdi, [rsp + 8]
    mov eax, 112                 ; SYS_SET_TLS (shared ABI)
    xor esi, esi
    xor edx, edx
    int 0x80
    and rsp, -16
    call pie_worker
    ud2
