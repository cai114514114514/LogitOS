#include "mce.h"
#include "hardware.h"
#include "panic.h"

#define MCA_VALID (1ull << 63)
#define MCA_UC    (1ull << 61)
#define MCA_OVER  (1ull << 62)
#define MCA_PCC   (1ull << 57)

/* #MC may interrupt a CPU holding kprintf/klog's lock. Raw polled serial
 * comes first, before panic can take over logging. It may interleave across
 * sockets but never waits for a lock owned by the interrupted context.
 * STATUS is always printed; ADDR/MISC are read only when their valid bits say
 * the MSRs contain information. Zero without a valid bit is not an address.
 * Intel SDM vol.3 chapter 15, Machine-Check Architecture:
 * https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html */
static void raw_text(const char *s) { while (*s) power_mce_putc(*s++); }
static void raw_number(uint64_t n, unsigned base, unsigned width)
{
    char digits[32]; unsigned count = 0;
    do { digits[count++] = "0123456789abcdef"[n % base]; n /= base; } while (n);
    while (count < width) digits[count++] = '0';
    while (count) power_mce_putc(digits[--count]);
}
static void mce_print_bank(int boot, unsigned bank, uint64_t status)
{
    raw_text(boot ? "[mce] boot-time bank " : "[mce] bank ");
    raw_number(bank, 10, 0); raw_text(" STATUS=0x"); raw_number(status, 16, 16);
    if (status & (1ull << 58)) {
        raw_text(" ADDR=0x"); raw_number(power_rdmsr(0x402 + bank * 4), 16, 16);
    } else raw_text(" ADDR=invalid");
    if (status & (1ull << 59)) {
        raw_text(" MISC=0x"); raw_number(power_rdmsr(0x403 + bank * 4), 16, 16);
    } else raw_text(" MISC=invalid");
    raw_text("\r\n");
}

static int mca_available(void)
{
    uint32_t r[4]; power_cpuid(1, 0, r);
    return (r[3] & ((1u << 5) | (1u << 7) | (1u << 14))) ==
           ((1u << 5) | (1u << 7) | (1u << 14));
}

static int scan_banks(int boot, unsigned count)
{
    int fatal = 0, found = 0;
    for (unsigned bank = 0; bank < count; bank++) {
        uint64_t status = power_rdmsr(0x401 + bank * 4);
        if (!(status & MCA_VALID)) continue;
        found = 1;
        mce_print_bank(boot, bank, status);
        /* UC is conservative: this kernel has no page offlining, poisoned
         * memory containment or instruction retry machinery. Even a hardware
         * signaled recoverable UC event must not be resumed by wishful logging.
         * OVER loses error information; PCC says context is corrupt. */
        if (!boot && (status & (MCA_UC | MCA_OVER | MCA_PCC))) fatal = 1;
        else power_wrmsr(0x401 + bank * 4, 0);
    }
    /* No valid record means the cause is unknown. RIPV alone does not prove
     * that an unclassified machine check can safely be ignored. */
    return fatal || (!boot && !found);
}

void mce_init(void)
{
    if (!mca_available()) return;
    uint64_t cap = power_rdmsr(0x179);
    unsigned count = cap & 255;
    (void)scan_banks(1, count);
    if (cap & (1ull << 8)) power_wrmsr(0x17b, UINT64_MAX);
    for (unsigned bank = 0; bank < count; bank++) {
        power_wrmsr(0x400 + bank * 4, UINT64_MAX);
        power_wrmsr(0x401 + bank * 4, 0);
    }
    power_write_cr(4, power_read_cr(4) | (1ull << 6));
}

void mce_handle(void)
{
    if (!mca_available()) {
        raw_text("[mce] MCA unavailable\r\n");
        panic("machine check without MCA");
    }
    uint64_t global = power_rdmsr(0x17a);
    int fatal = scan_banks(0, power_rdmsr(0x179) & 255);
    raw_text("[mce] MCG_STATUS=0x"); raw_number(global, 16, 16); raw_text("\r\n");
    if (fatal || !(global & 1)) panic("unrecoverable machine check");
    /* RIPV is mandatory before IRET. Clear MCIP last, after all diagnostics
     * and bank clearing; a nested #MC before this point is an architectural
     * shutdown, not something this handler pretends it can recover from. */
    power_wrmsr(0x17a, global & ~(1ull << 2));
}

#ifndef POWER_HOST_TEST
/* #MC bypasses generic exception-to-signal dispatch and its locks. The
 * 40-byte architectural frame + 15 registers + 528 bytes leaves the FXSAVE
 * image and SysV call aligned. Preserve SSE too: the kernel uses -msse2.
 * No error code is pushed for vector 18. DF is restored by IRET's RFLAGS. */
__attribute__((naked)) void mce_entry(void)
{
    __asm__ volatile (
        "push %rax; push %rcx; push %rdx; push %rsi; push %rdi;"
        "push %r8; push %r9; push %r10; push %r11;"
        "push %rbx; push %rbp; push %r12; push %r13; push %r14; push %r15;"
        "subq $528, %rsp; fxsave (%rsp); cld; call mce_handle;"
        "fxrstor (%rsp); addq $528, %rsp;"
        "pop %r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx;"
        "pop %r11; pop %r10; pop %r9; pop %r8;"
        "pop %rdi; pop %rsi; pop %rdx; pop %rcx; pop %rax; iretq");
}
#endif
