/* panic / assert / WARN, and the stack unwinder they all report through.
 *
 * The backtrace is the part that earns its keep, and the part that is easy to
 * get quietly wrong: at -O2 clang uses rbp as a general-purpose register, so
 * walking a "frame chain" in a default build reads whatever integers the code
 * happened to leave there and prints them as if they were call sites. The
 * kernel is therefore built with -fno-omit-frame-pointer (Makefile CFLAGS) --
 * and even then every frame is validated before it is believed:
 *
 *   - the frame pointer must be 8-aligned, at or above the current rsp, and
 *     inside the current stack (bounded, so we can never fault by following a
 *     wild pointer into unmapped memory);
 *   - it must move UPWARD every step, so a corrupted chain terminates instead
 *     of looping;
 *   - the return address must land inside the kernel image;
 *   - and the bytes in front of it must decode as a CALL. A return address
 *     always has a call immediately before it; a stale integer on the stack
 *     almost never does. Frames that fail this last check are still printed,
 *     marked '?', because suppressing them would hide real frames through
 *     assembly thunks -- but they are never presented as certain.
 *
 * When the frame chain yields almost nothing (asm frames, a smashed rbp), the
 * panic path falls back to a conservative STACK SCAN: every 8-byte slot in the
 * live stack that points into the kernel image and is preceded by a call is
 * listed as a candidate. That is noisier and says so.
 */

#include <stdint.h>
#include <stdarg.h>
#include "panic.h"
#include "klog.h"
#include "kprintf.h"
#include "vga.h"
#include "serial.h"
#include "percpu.h"
#include "interrupts.h"
#include "kdiag.h"

extern char _kernel_end[];          /* linker.ld: first byte past the image */

#define KIMAGE_LO   0x100000UL      /* linker.ld: . = 1M */
#define STACK_SPAN  (64 * 1024)     /* kernel stacks are 32 KiB; bound the walk
                                     * well inside the identity-mapped low
                                     * region so a wild pointer cannot fault */
#define BT_MAX      24

static volatile uint64_t g_warns;
static volatile uint64_t g_panics;
static char g_last_panic[128];
static volatile int g_in_panic;

uint64_t panic_warn_count(void) { return g_warns; }
uint64_t panic_count(void)      { return g_panics; }
const char *panic_last_message(void) { return g_last_panic; }

int kernel_text_contains(uint64_t a)
{
    /* Coarse on purpose: linker.ld exports only _kernel_end, so the upper
     * bound is the end of .bss rather than the end of .text. The call-site
     * check below is what makes the result trustworthy anyway. */
    return a >= KIMAGE_LO + 16 && a < (uint64_t)_kernel_end;
}

/* Does a CALL instruction end at `ret`? x86 calls are E8 rel32 (5 bytes) or
 * FF /2 with 0-3 bytes of prefix + modrm + optional SIB/disp (2-7 bytes). */
static int is_call_site(uint64_t ret)
{
    if (!kernel_text_contains(ret) || ret < KIMAGE_LO + 8)
        return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)ret;
    if (p[-5] == 0xE8)
        return 1;
    for (int back = 2; back <= 7; back++) {
        if (p[-back] == 0xFF && (((unsigned)p[-back + 1] >> 3) & 7u) == 2u)
            return 1;
    }
    return 0;
}

static int frame_ok(uint64_t fp, uint64_t rsp)
{
    return (fp & 7u) == 0 && fp >= rsp && fp + 16 <= rsp + STACK_SPAN;
}

int backtrace(uint64_t rbp, uint64_t rsp, uint64_t *out, int max)
{
    int n = 0;
    uint64_t fp = rbp;
    while (n < max && frame_ok(fp, rsp)) {
        uint64_t ret  = *(const uint64_t *)(uintptr_t)(fp + 8);
        uint64_t next = *(const uint64_t *)(uintptr_t)fp;
        if (!kernel_text_contains(ret))
            break;
        out[n++] = ret;
        if (next <= fp)             /* must grow: a cycle ends the walk */
            break;
        fp = next;
    }
    return n;
}

static void print_frame(int level, int i, uint64_t a)
{
    uint64_t off = 0;
    const char *sym = ksym_lookup ? ksym_lookup(a, &off) : (const char *)0;
    if (sym)
        klog(level, "  #%d %p  %s+0x%x%s", i, (void *)(uintptr_t)a, sym,
             (unsigned)off, is_call_site(a) ? "" : "  ?");
    else
        klog(level, "  #%d %p%s", i, (void *)(uintptr_t)a,
             is_call_site(a) ? "" : "  ?  (no call site here)");
}

void backtrace_print(int level, uint64_t rbp, uint64_t rsp, int max)
{
    uint64_t frames[BT_MAX];
    if (max > BT_MAX) max = BT_MAX;
    int n = backtrace(rbp, rsp, frames, max);

    if (n == 0) {
        klog(level, "  <no frame-pointer chain from rbp=%p>", (void *)(uintptr_t)rbp);
    } else {
        for (int i = 0; i < n; i++)
            print_frame(level, i, frames[i]);
    }

    /* A one- or zero-frame chain is usually an asm frame or a smashed rbp.
     * Fall back to a conservative scan rather than reporting nothing. */
    if (n <= 1) {
        klog(level, "  -- stack scan (candidates, may include stale returns) --");
        int shown = 0;
        for (uint64_t p = (rsp + 7) & ~7ull; p + 8 <= rsp + STACK_SPAN && shown < max; p += 8) {
            uint64_t v = *(const uint64_t *)(uintptr_t)p;
            if (!is_call_site(v))
                continue;
            uint64_t off = 0;
            const char *sym = ksym_lookup ? ksym_lookup(v, &off) : (const char *)0;
            if (sym)
                klog(level, "  ?%d %p  %s+0x%x", shown, (void *)(uintptr_t)v, sym, (unsigned)off);
            else
                klog(level, "  ?%d %p", shown, (void *)(uintptr_t)v);
            shown++;
        }
        if (!shown)
            klog(level, "  <stack scan found nothing>");
    }
}

void backtrace_here(int level, int max)
{
    uint64_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    backtrace_print(level, (uint64_t)(uintptr_t)__builtin_frame_address(0), rsp, max);
}

/* --- register capture -----------------------------------------------------
 *
 * Only the machine state that is still TRUE at this point is read here: the
 * flags, the segment selectors and the control registers. They are taken in a
 * single asm block, because each __asm__ statement is scheduled independently
 * and a dump assembled from a dozen separate blocks is not a snapshot of
 * anything.
 *
 * The general-purpose registers are deliberately NOT read. panic() is reached
 * by an ordinary call: rax/rcx/rdx/rsi/rdi/r8-r11 have already been overwritten
 * by the call sequence and by this function's own prologue, so reporting them
 * under a "registers" heading would be authoritative-looking fiction. They are
 * filled in only by panic_from_exception(), from the frame boot/isr.asm
 * pushed, and dump_regs() prints them only then. */
static void capture_sys(struct kregs *k)
{
    uint64_t flags, cs, ss, cr0, cr2, cr3, cr4;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "movq %%cs, %1\n\t"          /* MOV r64, Sreg: zero-extends. A 64-bit  */
        "movq %%ss, %2\n\t"          /* MEMORY destination is not encodable.   */
        "movq %%cr0, %3\n\t"
        "movq %%cr2, %4\n\t"
        "movq %%cr3, %5\n\t"
        "movq %%cr4, %6\n\t"
        : "=r"(flags), "=r"(cs), "=r"(ss),
          "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4)
        :: "memory");

    k->rflags = flags; k->cs = cs; k->ss = ss;
    k->cr0 = cr0; k->cr2 = cr2; k->cr3 = cr3; k->cr4 = cr4;
    k->err = 0;
    k->have_err = 0;
    k->have_gpr = 0;
    k->rax = k->rbx = k->rcx = k->rdx = k->rsi = k->rdi = 0;
    k->r8 = k->r9 = k->r10 = k->r11 = k->r12 = k->r13 = k->r14 = k->r15 = 0;
}

static void dump_regs(const struct kregs *k)
{
    klog(KL_PANIC, "RIP %p  RSP %p  RBP %p  RFLAGS %p",
         (void *)(uintptr_t)k->rip, (void *)(uintptr_t)k->rsp,
         (void *)(uintptr_t)k->rbp, (void *)(uintptr_t)k->rflags);
    if (k->have_err)
        klog(KL_PANIC, "CS  %p  SS  %p  error-code %p",
             (void *)(uintptr_t)k->cs, (void *)(uintptr_t)k->ss,
             (void *)(uintptr_t)k->err);
    else
        klog(KL_PANIC, "CS  %p  SS  %p", (void *)(uintptr_t)k->cs,
             (void *)(uintptr_t)k->ss);

    if (k->have_gpr) {
        klog(KL_PANIC, "RAX %p  RBX %p  RCX %p  RDX %p",
             (void *)(uintptr_t)k->rax, (void *)(uintptr_t)k->rbx,
             (void *)(uintptr_t)k->rcx, (void *)(uintptr_t)k->rdx);
        klog(KL_PANIC, "RSI %p  RDI %p  R8  %p  R9  %p",
             (void *)(uintptr_t)k->rsi, (void *)(uintptr_t)k->rdi,
             (void *)(uintptr_t)k->r8,  (void *)(uintptr_t)k->r9);
        klog(KL_PANIC, "R10 %p  R11 %p  R12 %p  R13 %p",
             (void *)(uintptr_t)k->r10, (void *)(uintptr_t)k->r11,
             (void *)(uintptr_t)k->r12, (void *)(uintptr_t)k->r13);
        klog(KL_PANIC, "R14 %p  R15 %p",
             (void *)(uintptr_t)k->r14, (void *)(uintptr_t)k->r15);
    } else {
        /* Say it out loud rather than printing the panic path's own registers
         * under a heading that implies they are the caller's. */
        klog(KL_PANIC, "GPRs: not captured -- deliberate panic()/KASSERT(), not a "
                       "trap; only a CPU exception has a register frame");
    }
    klog(KL_PANIC, "CR0 %p  CR2 %p  CR3 %p  CR4 %p",
         (void *)(uintptr_t)k->cr0, (void *)(uintptr_t)k->cr2,
         (void *)(uintptr_t)k->cr3, (void *)(uintptr_t)k->cr4);
}

/* --- the panic body ------------------------------------------------------- */

#define PANIC_BANNER "LOGIT_PANIC"   /* single grep-able marker for harnesses */

static void panic_body(const struct kregs *k, const char *what) __attribute__((noreturn));

static void panic_body(const struct kregs *k, const char *what)
{
    __asm__ volatile ("cli");

    /* Second panic while reporting the first (a fault inside the unwinder, a
     * panic on another core): do not try again, just stop. */
    if (g_in_panic) {
        for (;;)
            __asm__ volatile ("cli\n\thlt");
    }
    g_in_panic = 1;
    g_panics++;

    kdiag_stop_other_cpus();      /* the report is worthless if a live core
                                   * keeps overwriting the screen */

    klog_panic_takeover();        /* the dead core may hold the ring lock */
    klog_flush_partial();         /* the last line printed before the crash */

    uint64_t mark = klog_next_seq();

    vga_set_color(VGA_WHITE, VGA_RED);
    klog(KL_PANIC, "");
    klog(KL_PANIC, "*** " PANIC_BANNER " on cpu%d ***", this_cpu()->index);
    klog(KL_PANIC, "reason: %s", what && what[0] ? what : "(none given)");
    dump_regs(k);
    klog(KL_PANIC, "backtrace:");
    backtrace_print(KL_PANIC, k->rbp, k->rsp, BT_MAX);

    klog(KL_PANIC, "--- log ring (last 40 of %u records) ---",
         (unsigned)(mark - 1));
    klog_dump_tail_before(mark, 40);
    klog(KL_PANIC, "*** " PANIC_BANNER "_END -- halted, not rebooting ***");

    for (;;)
        __asm__ volatile ("cli\n\thlt");
}

void panic(const char *fmt, ...)
{
    struct kregs k;
    capture_sys(&k);
    /* rip = who called us, rbp/rsp = this frame -- all from builtins, so all
     * three genuinely describe the caller rather than the asm we just ran. */
    k.rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
    k.rbp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    __asm__ volatile ("movq %%rsp, %0" : "=r"(k.rsp));

    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(g_last_panic, (int)sizeof g_last_panic, fmt, ap);
    va_end(ap);

    panic_body(&k, g_last_panic);
}

void panic_assert_fail(const char *expr, const char *file, int line)
{
    struct kregs k;
    capture_sys(&k);
    k.rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
    k.rbp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    __asm__ volatile ("movq %%rsp, %0" : "=r"(k.rsp));

    ksnprintf(g_last_panic, (int)sizeof g_last_panic,
              "assertion failed: %s (%s:%d)", expr, file, line);
    panic_body(&k, g_last_panic);
}

void panic_from_exception(struct registers *r)
{
    struct kregs k;
    capture_sys(&k);                /* control registers (cr2 is the fault
                                     * address and is still live here) */
    k.have_gpr = 1;                 /* everything below IS the trap frame */

    k.rax = r->rax; k.rbx = r->rbx; k.rcx = r->rcx; k.rdx = r->rdx;
    k.rsi = r->rsi; k.rdi = r->rdi; k.rbp = r->rbp; k.rsp = r->rsp;
    k.r8  = r->r8;  k.r9  = r->r9;  k.r10 = r->r10; k.r11 = r->r11;
    k.r12 = r->r12; k.r13 = r->r13; k.r14 = r->r14; k.r15 = r->r15;
    k.rip = r->rip; k.rflags = r->rflags; k.cs = r->cs; k.ss = r->ss;
    k.err = r->error_code;
    k.have_err = 1;

    static const char *const names[32] = {
        "divide-by-zero", "debug", "NMI", "breakpoint",
        "overflow", "bound range", "invalid opcode", "device not available",
        "double fault", "coprocessor overrun", "invalid TSS", "segment not present",
        "stack-segment fault", "general protection", "page fault", "reserved",
        "x87 FP", "alignment check", "machine check", "SIMD FP",
        "virtualization", "control protection", "?", "?",
        "?", "?", "?", "?", "hypervisor", "VMM comm", "security", "?",
    };
    ksnprintf(g_last_panic, (int)sizeof g_last_panic,
              "CPU exception %u (%s) in kernel mode",
              (unsigned)r->vector, names[r->vector & 31]);
    panic_body(&k, g_last_panic);
}

/* --- WARN ----------------------------------------------------------------- */

void warn_report(const char *expr, const char *file, int line, const char *fmt, ...)
{
    g_warns++;

    char msg[96];
    msg[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(msg, (int)sizeof msg, fmt, ap);
        va_end(ap);
    }

    if (expr && msg[0])
        klog(KL_WARN, "%s:%d: WARN_ON(%s) -- %s", file, line, expr, msg);
    else if (expr)
        klog(KL_WARN, "%s:%d: WARN_ON(%s)", file, line, expr);
    else
        klog(KL_WARN, "%s:%d: %s", file, line, msg);

    uint64_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    uint64_t frames[6];
    int n = backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0), rsp, frames, 6);
    for (int i = 0; i < n; i++)
        print_frame(KL_WARN, i, frames[i]);
}
