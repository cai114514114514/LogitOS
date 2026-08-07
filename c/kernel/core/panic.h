#ifndef LOGIT_PANIC_H
#define LOGIT_PANIC_H

#include <stdint.h>

struct registers;   /* interrupts.h */

/* ---------------------------------------------------------------------------
 * panic / assert / WARN.
 *
 * A kernel that dies silently is a kernel you cannot debug on a machine you do
 * not own. The contract here:
 *
 *   panic()      prints a reason, a full register dump, a stack BACKTRACE and
 *                the tail of the log ring, stops the other cores, and halts.
 *                It does not reboot: a reboot loop erases the evidence.
 *   KASSERT()    a condition that must hold; failing it panics with the
 *                expression text and its file:line.
 *   WARN_ON()    a condition that should hold; failing it logs at KL_WARN with
 *                a short backtrace and CONTINUES, because most driver
 *                bring-up problems are not fatal and the machine is more
 *                useful alive.
 *
 * All of them are usable from interrupt context and from code holding any
 * lock: they allocate nothing and take only the log ring's leaf lock (and
 * panic() does not even take that -- see klog_panic_takeover).
 * ------------------------------------------------------------------------ */

/* Registers as the panic path reports them.
 *
 * `have_gpr` is the honesty bit, and it matters more than it looks. Only
 * panic_from_exception() has a TRAP FRAME -- registers as they were at the
 * faulting instruction. A deliberate panic()/KASSERT() is an ordinary C call:
 * by the time it runs, every caller-saved register belongs to the panic path
 * itself, and reading them back would print the logger's own state under a
 * heading that says "registers at the fault". So they are not printed at all
 * in that case, and the report says why. rip/rsp/rbp still are, because they
 * are taken from compiler builtins and do describe the caller. */
struct kregs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, cs, ss;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t err;
    int      have_err;      /* 1 = `err` came from a CPU exception */
    int      have_gpr;      /* 1 = the GPRs are a trap frame, not our own */
};

void panic(const char *fmt, ...) __attribute__((noreturn));
void panic_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));

/* THE HOOK c/kernel/cpu/interrupts.c NEEDS.
 *
 * A kernel-mode CPU exception currently lands in interrupts.c's static
 * panic_exception(), which prints six values and spins. That file belongs to
 * another line, so this function is provided but NOT yet called. Wiring it up
 * is one line there:
 *
 *     static void panic_exception(struct registers *r) { panic_from_exception(r); }
 *
 * (plus #include "panic.h"). Until that lands, a kernel fault still produces
 * the old two-line report with no backtrace and no log dump. */
void panic_from_exception(struct registers *r) __attribute__((noreturn));

/* Report-and-continue. `expr` may be NULL (a bare WARN with a message);
 * `fmt` may be NULL (a bare WARN_ON of a condition). */
void warn_report(const char *expr, const char *file, int line, const char *fmt, ...);

/* --- stack unwinding ------------------------------------------------------
 * Frame-pointer walk. The kernel is built with -fno-omit-frame-pointer for
 * exactly this reason (see the Makefile): at -O2 clang otherwise uses rbp as a
 * general register and the "chain" is arbitrary data. Every candidate is
 * validated before it is believed -- the frame pointer must stay inside the
 * current stack and move upward, and the return address must land in the
 * kernel image AND be preceded by a call instruction. A frame that fails the
 * call-site check is printed with a '?', never silently as fact. */
int  backtrace(uint64_t rbp, uint64_t rsp, uint64_t *out, int max);
void backtrace_print(int level, uint64_t rbp, uint64_t rsp, int max);
void backtrace_here(int level, int max);      /* unwind from the caller's frame */

int  kernel_text_contains(uint64_t addr);

/* --- counters, for the runtime introspection surface --------------------- */
uint64_t panic_warn_count(void);
uint64_t panic_count(void);
const char *panic_last_message(void);         /* "" if none */

/* Optional symbol resolution. Weak: if no symbol table is linked in, the
 * backtrace prints bare addresses to be resolved against build/kernel.map. */
const char *ksym_lookup(uint64_t addr, uint64_t *offset) __attribute__((weak));

#define KASSERT(cond)                                                        \
    do {                                                                     \
        if (__builtin_expect(!(cond), 0))                                    \
            panic_assert_fail(#cond, __FILE__, __LINE__);                    \
    } while (0)

/* Same, with context. `panic` formats the message, so this is how an assertion
 * says WHICH index was out of range rather than only that one was. */
#define KASSERT_MSG(cond, ...)                                               \
    do {                                                                     \
        if (__builtin_expect(!(cond), 0)) {                                  \
            warn_report(#cond, __FILE__, __LINE__, __VA_ARGS__);             \
            panic_assert_fail(#cond, __FILE__, __LINE__);                    \
        }                                                                    \
    } while (0)

/* Evaluates to the condition, so it composes: `if (WARN_ON(!dev)) return -1;` */
#define WARN_ON(cond)                                                        \
    ({ int _w = !!(cond);                                                    \
       if (__builtin_expect(_w, 0))                                          \
           warn_report(#cond, __FILE__, __LINE__, (const char *)0);          \
       _w; })

#define WARN_ON_ONCE(cond)                                                   \
    ({ static int _warned; int _w = !!(cond);                                \
       if (__builtin_expect(_w && !_warned, 0)) {                            \
           _warned = 1;                                                      \
           warn_report(#cond, __FILE__, __LINE__, "(once)");                 \
       }                                                                     \
       _w; })

#define WARN(...) warn_report((const char *)0, __FILE__, __LINE__, __VA_ARGS__)

#endif /* LOGIT_PANIC_H */
