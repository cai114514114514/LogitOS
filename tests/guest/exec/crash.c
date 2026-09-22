#include "clib.h"

/* crash MODE [ADDR]      fault on purpose, with known values in the registers
 *
 *   crash segv [ADDR]    store to ADDR (default 0xdeadbee0) -- vector 14
 *   crash read [ADDR]    load  from ADDR                    -- vector 14, err bit 1 clear
 *   crash ill            ud2                                -- vector 6
 *   crash div0           idiv by zero                       -- vector 0
 *   crash ok             do none of that and exit 0
 *
 * WHAT IT IS FOR, and it is not "test that crashing works". A core dump is
 * only worth anything if the register file in it is the register file the
 * program had, and there is no way to check that against a program whose
 * registers nobody chose: every value in a crashing program is whatever the
 * compiler happened to leave there, so "the dump says r13 was 0x7ffd0128" can
 * be compared with nothing. This program puts a value NOBODY ELSE WOULD
 * PRODUCE into each register, prints them, and then faults with them still
 * live -- so the dump's register file has an oracle that was written down
 * before the fault happened, on a different channel from the dump.
 *
 * `crash ok` is the control for the harness one level up: the same binary,
 * the same path, no fault -- so "a dump appeared" cannot be satisfied by
 * anything that runs this program.
 *
 * THE SENTINELS ARE LOADED IN ONE ASM BLOCK ENDING IN THE FAULT. Nothing may
 * run between the loads and the trap or the compiler is entitled to reuse the
 * registers, and it would: r12..r15 and rbx are callee-saved, which is exactly
 * why they are the ones chosen -- a caller-saved register would be clobbered
 * by any call the fault path made before the dump.
 *
 * rbp IS DELIBERATELY NOT A SENTINEL. The build uses -fno-omit-frame-pointer,
 * so naming rbp in an asm clobber list is rejected outright by both compilers
 * this tree uses; the alternative (a naked function, or hand-written asm in
 * its own .asm file) buys one more register at the cost of a build rule. Six
 * sentinels are already more than the property needs. */

#define S_R12 0xC0DE000000000012ULL
#define S_R13 0xC0DE000000000013ULL
#define S_R14 0xC0DE000000000014ULL
#define S_R15 0xC0DE000000000015ULL
#define S_RBX 0xC0DE0000000000B8ULL
#define S_RDI 0xC0DE0000000000D1ULL

static volatile unsigned long S[8];

static void hex(unsigned long v)
{
    char t[20]; int k = 0;
    const char *d = "0123456789abcdef";
    if (!v) t[k++] = '0';
    while (v) { t[k++] = d[v & 15]; v >>= 4; }
    outs("0x");
    while (k) outc(t[--k]);
}

static unsigned long parse_hex(const char *s)
{
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c >= '0' && c <= '9')      v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
    }
    return v;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "segv";
    unsigned long addr = argc > 2 ? parse_hex(argv[2]) : 0xdeadbee0UL;

    if (c_streq(mode, "ok")) { outs("CRASH ok -- no fault\n"); return 0; }

    S[0] = S_R12; S[1] = S_R13; S[2] = S_R14; S[3] = S_R15;
    S[4] = S_RBX; S[5] = S_RDI; S[6] = addr;

    /* Printed BEFORE the fault and on the same channel the kernel's [fault]
     * and [core] lines use, so the harness reads the expectation and the
     * result out of one log in order. */
    outs("CRASH mode "); outs(mode);
    outs(" addr "); hex(addr);
    outs(" r12 "); hex(S_R12); outs(" r13 "); hex(S_R13);
    outs(" r14 "); hex(S_R14); outs(" r15 "); hex(S_R15);
    outs(" rbx "); hex(S_RBX); outs(" rdi "); hex(S_RDI);
    outc('\n');

    if (c_streq(mode, "ill")) {
        __asm__ volatile (
            "movq 0(%0), %%r12\n\t" "movq 8(%0), %%r13\n\t"
            "movq 16(%0), %%r14\n\t" "movq 24(%0), %%r15\n\t"
            "movq 32(%0), %%rbx\n\t" "movq 40(%0), %%rdi\n\t"
            "ud2\n\t"
            :: "r"(S) : "r12","r13","r14","r15","rbx","rdi","memory");
    } else if (c_streq(mode, "div0")) {
        __asm__ volatile (
            "movq 0(%0), %%r12\n\t" "movq 8(%0), %%r13\n\t"
            "movq 16(%0), %%r14\n\t" "movq 24(%0), %%r15\n\t"
            "movq 32(%0), %%rbx\n\t" "movq 40(%0), %%rdi\n\t"
            "xorq %%rdx, %%rdx\n\t" "movq $1, %%rax\n\t"
            "xorq %%rcx, %%rcx\n\t" "idivq %%rcx\n\t"
            :: "r"(S) : "r12","r13","r14","r15","rbx","rdi",
                        "rax","rcx","rdx","memory");
    } else if (c_streq(mode, "read")) {
        __asm__ volatile (
            "movq 0(%0), %%r12\n\t" "movq 8(%0), %%r13\n\t"
            "movq 16(%0), %%r14\n\t" "movq 24(%0), %%r15\n\t"
            "movq 32(%0), %%rbx\n\t" "movq 40(%0), %%rdi\n\t"
            "movq 48(%0), %%rcx\n\t"
            "movq (%%rcx), %%rax\n\t"
            :: "r"(S) : "r12","r13","r14","r15","rbx","rdi","rax","rcx","memory");
    } else {
        __asm__ volatile (
            "movq 0(%0), %%r12\n\t" "movq 8(%0), %%r13\n\t"
            "movq 16(%0), %%r14\n\t" "movq 24(%0), %%r15\n\t"
            "movq 32(%0), %%rbx\n\t" "movq 40(%0), %%rdi\n\t"
            "movq 48(%0), %%rcx\n\t"
            "movq %%rcx, (%%rcx)\n\t"
            :: "r"(S) : "r12","r13","r14","r15","rbx","rdi","rcx","memory");
    }

    /* Reached only if the fault did not happen, which is itself a finding:
     * the address was mapped, or the instruction was emulated away. Said out
     * loud rather than returning 0, because a harness whose fixture quietly
     * did not fault would report the dump as missing. */
    outs("CRASH DID NOT FAULT -- the address was writable\n");
    return 2;
}
