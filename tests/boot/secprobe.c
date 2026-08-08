/* /bin/secprobe -- a deliberately hostile ring-3 program.
 *
 * WHAT THIS IS FOR
 * ----------------
 * The only convincing test of a memory-protection boundary is an attempt to
 * cross it that USED TO SUCCEED and now faults. An assertion that the bit is
 * set proves the bit is set; it does not prove the bit does anything, and this
 * kernel spent a long time DETECTING SMEP and SMAP in c/kernel/cpu/cpufeat.c
 * while setting neither in CR4 -- a state that reads, from outside, exactly
 * like a protection that is working.
 *
 * So each subcommand below is an exploit. Each one prints a marker line saying
 * what happened, and the harness (tests/boot/run-sec-test.sh) asserts on which
 * marker appeared. Two markers per attack, and they are the whole design:
 *
 *     SEC-<name>-PWNED     the attack worked. The boundary is open.
 *     SEC-<name>-BLOCKED   the kernel refused it and we lived to say so.
 *
 * and a third that is not printed by this program at all: if the attack is
 * blocked by a FAULT rather than by a return value, this process dies and the
 * kernel prints "[fault] app exception ... terminating app". The harness treats
 * that as the block. That is why every attack gets its own process -- a fault
 * is fatal to the whole program, so a probe that tried two attacks in one run
 * could only ever report on the first.
 *
 * ONE ATTACK PER INVOCATION. `secprobe wx`, `secprobe nx`, and so on.
 *
 * The `nx` case is expected to print PWNED right now, and the harness expects
 * it to. That is not a broken test: NX is not on (c/kernel/cpu/prot.h explains
 * that it is blocked on c/kernel/mm's PTE->frame masks keeping bit 63), and a
 * test suite whose every case passes cannot tell you which protections you
 * actually have. When NX lands, that expectation flips in the harness and this
 * file does not change.
 */

#include "logit.h"
#include "clib.h"

/* A ring-3 program's own text base. Every CLI program in this system links at
 * 0x50000000 (Makefile CLI_RULE -Ttext), and the first PT_LOAD with PF_X starts
 * there, so this address is inside our own code -- specifically in crt0_cli's
 * _start, which has already run and is never re-entered, so overwriting it does
 * not corrupt anything we are about to execute. The point is only whether the
 * WRITE is permitted. */
#define OWN_TEXT 0x50000000UL

/* A kernel address. The kernel is loaded at 1 MiB and the low 1 GiB is identity
 * mapped with 2 MiB pages that carry no USER bit, so ring 3 must not be able to
 * read this, and no syscall must be willing to write to it on our behalf. */
#define KERNEL_ADDR 0x00100000UL

/* Something in .rodata: read-only by the ELF's own p_flags. */
static const char rodata_probe[] = "rodata";

/* Every marker is emitted as ONE sys_write, newline included.
 *
 * Not a style preference. stdout here is the serial console, which the kernel
 * also logs to, and the kernel logs from interrupt handlers. A marker printed
 * as outs("SEC-begin "); outs(name); outs("\n") really does come out as
 *
 *     SEC-begin [waitq] 200 ms of waiting: ...
 *     kread
 *
 * -- observed, not imagined -- and the harness's line-anchored grep then never
 * matches, so a correctly blocked attack is scored DIDNOTRUN. One write per
 * line makes the interleaving happen BETWEEN markers instead of inside one. */
static void say2(const char *a, const char *b)
{
    char line[96];
    int n = 0;
    for (int i = 0; a[i] && n < (int)sizeof line - 2; i++) line[n++] = a[i];
    for (int i = 0; b && b[i] && n < (int)sizeof line - 2; i++) line[n++] = b[i];
    line[n++] = '\n';
    sys_write(1, line, n);
}
static void say(const char *s) { say2(s, 0); }

/* --- the attacks --------------------------------------------------------- */

/* W^X, write half: overwrite our own code.
 * Before honest ELF permissions, elf_load() mapped every PT_LOAD
 * VMM_WRITABLE|VMM_USER whatever p_flags said, so this simply worked. */
static void attack_wx(void)
{
    volatile unsigned char *code = (volatile unsigned char *)OWN_TEXT;
    unsigned char was = *code;              /* reading our own text is fine */
    *code = (unsigned char)(was ^ 0xFF);    /* <- must fault */
    /* Only reached if the store was allowed. Read it back rather than trusting
     * the store: a write that is silently dropped is still a failure to fault,
     * but it is a different one and worth telling apart. */
    if (*code == was) say("SEC-wx-PWNED-nofault-nostore");
    else              say("SEC-wx-PWNED");
}

/* W^X, write half again, on .rodata rather than .text. Separate because they
 * are separate PT_LOADs with separate p_flags, and a loader that got .text
 * right by special-casing the entry point would still get this wrong. */
static void attack_rodata(void)
{
    volatile char *p = (volatile char *)(unsigned long)rodata_probe;
    char was = *p;
    *p = (char)(was ^ 0xFF);                /* <- must fault */
    if (*p == was) say("SEC-rodata-PWNED-nofault-nostore");
    else           say("SEC-rodata-PWNED");
}

/* NX: write machine code into a data page and jump to it.
 * The bytes are `mov eax, 1; ret` -- a function returning 1, so a successful
 * jump is observable as a return value and not merely as "we did not crash".
 * The buffer is static (.bss, a writable PT_LOAD) rather than on the stack, so
 * this tests the loader's data mapping specifically. */
static unsigned char shellcode[16];
static void attack_nx(void)
{
    shellcode[0] = 0xB8; shellcode[1] = 0x01;      /* mov eax, 1 */
    shellcode[2] = 0x00; shellcode[3] = 0x00; shellcode[4] = 0x00;
    shellcode[5] = 0xC3;                            /* ret */
    int (*fn)(void) = (int (*)(void))(void *)shellcode;
    int r = fn();                                   /* <- must fault */
    if (r == 1) say("SEC-nx-PWNED");
    else        say("SEC-nx-PWNED-ran-wrong");
}

/* NX on the stack, which is where a real overflow puts its payload. Same code,
 * different page class: the stack comes from setup_cli_stack()/do_anon(), not
 * from a PT_LOAD, so it is mapped by different code and can differ. */
static void attack_nx_stack(void)
{
    volatile unsigned char buf[16];
    buf[0] = 0xB8; buf[1] = 0x01; buf[2] = 0x00; buf[3] = 0x00; buf[4] = 0x00;
    buf[5] = 0xC3;
    int (*fn)(void) = (int (*)(void))(void *)buf;
    int r = fn();                                   /* <- must fault */
    if (r == 1) say("SEC-nxstack-PWNED");
    else        say("SEC-nxstack-PWNED-ran-wrong");
}

/* Syscall argument validation: hand the kernel a KERNEL pointer and ask it to
 * write there. If the kernel dereferences an unvalidated user pointer, this is
 * a full compromise from an unprivileged process -- it overwrites kernel code
 * with the current working directory.
 *
 * SYS_GETCWD is the sharpest probe available: it takes a pointer and a length
 * and writes a NUL-terminated string, with no other side effect to disentangle.
 * Three separate targets, because a check that only compares against a base
 * address would pass one and fail another:
 *   - the kernel image at 1 MiB (mapped, supervisor-only)
 *   - a non-canonical address (would #GP inside the kernel, which is fatal here)
 *   - a user address whose START is valid but which runs off the end of the
 *     mapped region, which is the classic length-check miss. */
static void attack_kptr(void)
{
    long a = sys_getcwd((char *)KERNEL_ADDR, 64);
    long b = sys_getcwd((char *)0x0000800000000000UL, 64);   /* non-canonical */
    /* Straddle: start inside our own .bss, length far past anything mapped. */
    long c = sys_getcwd((char *)(void *)shellcode, 1 << 28);

    if (a >= 0) { say("SEC-kptr-PWNED-kernel"); return; }
    if (b >= 0) { say("SEC-kptr-PWNED-noncanonical"); return; }
    if (c >= 0) { say("SEC-kptr-PWNED-straddle"); return; }
    say("SEC-kptr-BLOCKED");
}

/* The same idea through a different syscall family, because the validation is
 * per-call-site and one audited site says nothing about the next. SYS_READ into
 * a kernel buffer, and SYS_WRITE *from* one (an information leak rather than a
 * corruption: it would print kernel memory to our stdout). */
static void attack_kptr_rw(void)
{
    int fd = sys_open("/docs/readme.txt", 0);
    long r = (fd >= 0) ? sys_read(fd, (char *)KERNEL_ADDR, 64) : -1;
    if (fd >= 0) sys_close(fd);
    long w = sys_write(1, (const char *)KERNEL_ADDR, 64);

    if (r >= 0) { say("SEC-kptrrw-PWNED-read-into-kernel"); return; }
    if (w >= 0) { say("SEC-kptrrw-PWNED-leak-kernel"); return; }
    say("SEC-kptrrw-BLOCKED");
}

/* Direct read of kernel memory from ring 3 -- no syscall involved, just a load.
 * This is the baseline the whole user/kernel split rests on: if it works,
 * nothing else on the list matters. Expected to fault. */
static void attack_kread(void)
{
    volatile unsigned long *p = (volatile unsigned long *)KERNEL_ADDR;
    unsigned long v = *p;                           /* <- must fault */
    if (v) say("SEC-kread-PWNED");
    else   say("SEC-kread-PWNED-zero");
}

int main(int argc, char **argv)
{
    if (argc < 2) { say("usage: secprobe <wx|rodata|nx|nxstack|kptr|kptrrw|kread>"); return 2; }
    const char *w = argv[1];

    /* Announce BEFORE attacking. If the attack faults, this line is the only
     * evidence that the probe ran at all, and without it a probe that failed to
     * launch would be indistinguishable from one that was correctly blocked --
     * which would make the whole suite pass for the wrong reason. */
    say2("SEC-begin ", w);

    if      (c_streq(w, "wx"))      attack_wx();
    else if (c_streq(w, "rodata"))  attack_rodata();
    else if (c_streq(w, "nx"))      attack_nx();
    else if (c_streq(w, "nxstack")) attack_nx_stack();
    else if (c_streq(w, "kptr"))    attack_kptr();
    else if (c_streq(w, "kptrrw"))  attack_kptr_rw();
    else if (c_streq(w, "kread"))   attack_kread();
    else { say("SEC-unknown"); return 2; }

    /* Reached only when the attack neither faulted nor was refused, or when the
     * attack reported a refusal itself. The per-attack marker above carries the
     * verdict; this just proves we got to the end under our own power. */
    say2("SEC-end ", w);
    return 0;
}
