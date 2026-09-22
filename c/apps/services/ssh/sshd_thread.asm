; ============================================================================
; The first few instructions of every helper thread sshd.c spawns.
;
; sshd is a CLI program (CLI_RULE / the pkgverify/login shape): it links no
; libc, so it cannot reuse c/apps/libc/src/pthread_entry.asm -- that stub's
; `call __logit_thread_body` is a mini-libc symbol this program does not link,
; and pulling in the rest of pthread.c to get one function would mean linking
; the 8 MiB-stack, TLS-segment threading layer for a program that manages its
; own fixed, small, caller-owned per-connection stacks on purpose (see the
; long comment in sshd.c on why: thread-per-connection here needs 2-3 threads
; per connection and the practical ceiling on kernel-mmap'd thread stacks is
; ~13 per process -- VMA_MAXAREA, not the thread table -- so sshd hands the
; kernel a stack it already owns, which sidesteps that ceiling entirely).
;
; So this is the SAME stub, mechanically: c/kernel/sched writes
; [rsp] = arg, [rsp+8] = tls at the top of a new thread's stack before the
; first IRETQ (see include/abi/logit_abi.h's SYS_THREAD_CREATE and the
; identical comment in pthread_entry.asm, which this file is a deliberate
; twin of rather than a reinvention). sshd's threads carry no TLS (they are
; plain C, no __thread anywhere), so `tls` is always 0 and the SYS_SET_TLS
; call below is a documented no-op kept only because skipping it would make
; this stub depend on an unstated kernel guarantee ("a fresh thread's %fs
; base already reads as 0") rather than an ASSERTED one.
; ============================================================================

global sshd_thread_entry
extern sshd_thread_body

section .text
bits 64

sshd_thread_entry:
    mov  rdi, [rsp + 8]         ; tls (always 0 here)
    mov  rax, 112               ; SYS_SET_TLS -- see pthread_entry.asm's note
    int  0x80

    mov  rdi, [rsp]             ; arg: a (struct sshd_thread_arg *)
    and  rsp, -16               ; IRETQ entry established no call-site alignment
    call sshd_thread_body

; sshd_thread_body ends in SYS_THREAD_EXIT and never returns. If it somehow
; did, falling off the end of a borrowed stack would be a wild jump into
; whatever sshd.c's .bss holds next -- stop here instead, loudly, on purpose.
.hang:
    hlt
    jmp .hang
