/* kdiag -- /dev/kmsg, /dev/kstat, /dev/ktrigger, and the interrupt-context
 * proof for the log ring. See kdiag.h for why this is a file and not a
 * syscall. */

#include <stdint.h>
#include <stddef.h>
#include "kdiag.h"
#include "klog.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
#include "spinlock.h"
#include "lapic.h"
#include "smp.h"
#include "pit.h"
#include "pmm.h"
#include "sched.h"
#include "proc.h"

/* Diagnostic interrupt vectors. 0..47 are the CPU exceptions + the remapped
 * PIC IRQs, 65 is the NIC, 128 is int 0x80, 240/241 are the TLB and present
 * IPIs and 255 is LAPIC spurious -- 238/239 are free. Both gates are installed
 * by kdiag_init() through idt_install_gate(); they route STRAIGHT to the stubs
 * below rather than through interrupts.c's dispatcher, so they take no BKL and
 * cannot deadlock against whatever the interrupted core was doing. */
#define KDIAG_VEC_LOG   239
#define KDIAG_VEC_HALT  238

void idt_install_gate(int vec, void *handler);   /* c/kernel/cpu/idt.c */

/* --- synthetic file plumbing --------------------------------------------- */

static char g_snap[96 * 1024];      /* .bss: rendering never allocates */
static int  g_snap_len;
static char g_snap_for[32];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void scopy(char *d, const char *s, int max)
{
    int i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static int render_kstat(char *buf, int max);

static int render(const char *path)
{
    if (streq(path, "/dev/kmsg"))
        g_snap_len = klog_render(g_snap, (int)sizeof g_snap);
    else if (streq(path, "/dev/kstat"))
        g_snap_len = render_kstat(g_snap, (int)sizeof g_snap);
    else
        return -1;
    scopy(g_snap_for, path, sizeof g_snap_for);
    return g_snap_len;
}

int kdiag_size(const char *path)
{
    if (!path) return KDIAG_NOT_MINE;
    if (streq(path, "/dev/ktrigger")) return 0;
    if (streq(path, "/dev/kmsg") || streq(path, "/dev/kstat"))
        return render(path);
    return KDIAG_NOT_MINE;
}

int kdiag_read(const char *path, void *buf, int max)
{
    if (!path) return KDIAG_NOT_MINE;
    if (streq(path, "/dev/ktrigger")) return 0;
    if (!streq(path, "/dev/kmsg") && !streq(path, "/dev/kstat"))
        return KDIAG_NOT_MINE;

    /* file.c calls vfs_size() then vfs_read(); serve the SAME snapshot to both
     * so a log line arriving between the two calls cannot make the read
     * disagree with the length the caller sized its buffer from. */
    if (!streq(g_snap_for, path))
        render(path);

    int n = g_snap_len < max ? g_snap_len : max;
    char *out = (char *)buf;
    for (int i = 0; i < n; i++)
        out[i] = g_snap[i];
    return n;
}

/* --- /dev listing --------------------------------------------------------- */

static const char *const g_devnames[] = { "kmsg", "kstat", "ktrigger" };
#define NDEV ((int)(sizeof g_devnames / sizeof g_devnames[0]))

int kdiag_dir_count(const char *dir)
{
    if (dir && (streq(dir, "/dev") || streq(dir, "/dev/")))
        return NDEV;
    return KDIAG_NOT_MINE;
}

const char *kdiag_dir_name(const char *dir, int i)
{
    if (dir && (streq(dir, "/dev") || streq(dir, "/dev/")) && i >= 0 && i < NDEV)
        return g_devnames[i];
    return 0;
}

int kdiag_dir_size(const char *dir, int i)
{
    if (!dir || !(streq(dir, "/dev") || streq(dir, "/dev/")) || i < 0 || i >= NDEV)
        return KDIAG_NOT_MINE;
    char p[32];
    ksnprintf(p, sizeof p, "/dev/%s", g_devnames[i]);
    int n = kdiag_size(p);
    return n < 0 ? 0 : n;
}

/* --- /dev/kstat: what the system thinks its state is ---------------------- */

static int render_kstat(char *buf, int max)
{
    struct klog_stats st;
    klog_get_stats(&st);

    uint64_t ms = timer_ms();
    uint64_t tot = pmm_total_bytes(), fre = pmm_free_bytes();
    int n = 0;

    n += ksnprintf(buf + n, max - n, "logit kstat\n");
    n += ksnprintf(buf + n, max - n, "uptime_ms          %llu\n", (unsigned long long)ms);
    n += ksnprintf(buf + n, max - n, "ticks              %llu\n", (unsigned long long)timer_ticks());
    n += ksnprintf(buf + n, max - n, "timer_hz           %u\n", (unsigned)TIMER_HZ);
    n += ksnprintf(buf + n, max - n, "cpus_online        %d\n", smp_cpu_count());
    n += ksnprintf(buf + n, max - n, "ctx_switches       %llu\n",
                   (unsigned long long)sched_switches());
    n += ksnprintf(buf + n, max - n, "mem_total_bytes    %llu\n", (unsigned long long)tot);
    n += ksnprintf(buf + n, max - n, "mem_free_bytes     %llu\n", (unsigned long long)fre);
    n += ksnprintf(buf + n, max - n, "mem_used_bytes     %llu\n",
                   (unsigned long long)(tot - fre));
    n += ksnprintf(buf + n, max - n, "klog_records       %llu\n",
                   (unsigned long long)st.records);
    n += ksnprintf(buf + n, max - n, "klog_capacity      %u\n", st.slots);
    n += ksnprintf(buf + n, max - n, "klog_text_max      %u\n", st.text_max);
    n += ksnprintf(buf + n, max - n, "klog_aged_out      %llu\n",
                   (unsigned long long)st.overwritten);
    n += ksnprintf(buf + n, max - n, "klog_trunc_lines   %llu\n",
                   (unsigned long long)st.truncated_lines);
    n += ksnprintf(buf + n, max - n, "klog_trunc_chars   %llu\n",
                   (unsigned long long)st.truncated_chars);
    n += ksnprintf(buf + n, max - n, "klog_console_level %d\n", klog_console_level());
    n += ksnprintf(buf + n, max - n, "log_panic          %llu\n",
                   (unsigned long long)st.by_level[KL_PANIC]);
    n += ksnprintf(buf + n, max - n, "log_err            %llu\n",
                   (unsigned long long)st.by_level[KL_ERR]);
    n += ksnprintf(buf + n, max - n, "log_warn           %llu\n",
                   (unsigned long long)st.by_level[KL_WARN]);
    n += ksnprintf(buf + n, max - n, "log_info           %llu\n",
                   (unsigned long long)st.by_level[KL_INFO]);
    n += ksnprintf(buf + n, max - n, "log_debug          %llu\n",
                   (unsigned long long)st.by_level[KL_DEBUG]);
    n += ksnprintf(buf + n, max - n, "warns              %llu\n",
                   (unsigned long long)panic_warn_count());
    n += ksnprintf(buf + n, max - n, "panics             %llu\n",
                   (unsigned long long)panic_count());
    n += ksnprintf(buf + n, max - n, "last_panic         %s\n",
                   panic_last_message()[0] ? panic_last_message() : "(none)");

    n += ksnprintf(buf + n, max - n, "\npid  ppid state name\n");
    for (int pid = 1; pid <= NPROC * 4 && n < max - 64; pid++) {
        struct proc *p = proc_by_pid(pid);
        if (!p || p->state == 0)
            continue;
        n += ksnprintf(buf + n, max - n, "%-4d %-4d %-5s %s\n",
                       p->pid, p->ppid,
                       p->state == 1 ? "run" : "zomb", p->name);
    }
    return n;
}

/* --- interrupt-context logging probe -------------------------------------
 * The claim "klog is safe from an interrupt handler" is only worth something
 * if an interrupt handler actually logs. This installs a real IDT gate and
 * drives it with LAPIC IPIs, which arrive ASYNCHRONOUSLY -- at an arbitrary
 * instruction, including inside another klog call on the same core and while
 * another core holds the ring lock. That is the shape of the failure being
 * excluded, so that is the shape of the test.
 *
 * Each record carries the same counter twice ("n=K ... tail=K"). A record
 * whose two halves disagree, or whose text is malformed, is a torn record --
 * which is exactly what a shared line buffer or an unguarded append would
 * produce. The verdict line reports the count. */

static volatile uint64_t g_irq_hits[PERCPU_MAXCPU];
static volatile uint64_t g_irq_total;

void kdiag_log_isr_body(void);          /* called from the naked stub below */
void kdiag_log_isr_body(void)
{
    int cpu = this_cpu()->index;
    if (cpu < 0 || cpu >= PERCPU_MAXCPU) cpu = 0;
    uint64_t n = ++g_irq_hits[cpu];
    __atomic_fetch_add(&g_irq_total, 1, __ATOMIC_RELAXED);
    klog(KL_DEBUG, "kdiag irq cpu=%d n=%llu tail=%llu",
         cpu, (unsigned long long)n, (unsigned long long)n);
    lapic_eoi();
}

/* Raw interrupt entry. Written by hand rather than reached through
 * boot/isr.asm because that path takes the BKL, and this probe must be able to
 * land on a core that is already inside the kernel.
 *
 * Alignment: the CPU aligns rsp to 16 before pushing the 40-byte interrupt
 * frame, so rsp = 8 (mod 16) here; 15 pushes (120 bytes) bring it back to 0,
 * and the 528-byte fxsave area keeps it there -- which is both what fxsave
 * requires and what the SysV ABI wants at a `call`. The fxsave/fxrstor is not
 * optional: the kernel is built with -msse2, this gate bypasses isr_common's
 * FXSAVE, and clobbering XMM state under a ring-3 app that is mid-computation
 * would be a corruption introduced BY the diagnostic. */
__attribute__((naked)) static void kdiag_log_isr(void)
{
    __asm__ volatile (
        "push %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %rsi\n\tpush %rdi\n\t"
        "push %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\t"
        "push %rbx\n\tpush %rbp\n\tpush %r12\n\tpush %r13\n\tpush %r14\n\tpush %r15\n\t"
        "subq $528, %rsp\n\t"
        "fxsave (%rsp)\n\t"
        "call kdiag_log_isr_body\n\t"
        "fxrstor (%rsp)\n\t"
        "addq $528, %rsp\n\t"
        "pop %r15\n\tpop %r14\n\tpop %r13\n\tpop %r12\n\tpop %rbp\n\tpop %rbx\n\t"
        "pop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\t"
        "pop %rdi\n\tpop %rsi\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\t"
        "iretq\n\t");
}

__attribute__((naked)) static void kdiag_halt_isr(void)
{
    __asm__ volatile ("1:\n\tcli\n\thlt\n\tjmp 1b\n\t");
}

void kdiag_init(void)
{
    idt_install_gate(KDIAG_VEC_LOG,  (void *)kdiag_log_isr);
    idt_install_gate(KDIAG_VEC_HALT, (void *)kdiag_halt_isr);
}

void kdiag_stop_other_cpus(void)
{
    if (!lapic_ready())
        return;
    int me = this_cpu()->index;
    int n = smp_cpu_count();
    if (n > PERCPU_MAXCPU) n = PERCPU_MAXCPU;
    for (int i = 0; i < n; i++) {
        if (i == me)
            continue;
        lapic_send_ipi((uint8_t)g_cpus[i].lapic_id, KDIAG_VEC_HALT);
    }
    for (volatile int spin = 0; spin < 4000000; spin++)
        ;                                   /* let them take the interrupt */
}

/* --- ring verification ---------------------------------------------------- */

static long field_of(const char *s, int len, const char *key)
{
    int klen = 0;
    while (key[klen]) klen++;
    for (int i = 0; i + klen < len; i++) {
        int j = 0;
        while (j < klen && s[i + j] == key[j]) j++;
        if (j != klen)
            continue;
        long v = 0;
        int k = i + klen, any = 0;
        while (k < len && s[k] >= '0' && s[k] <= '9') { v = v * 10 + (s[k] - '0'); k++; any = 1; }
        return any ? v : -1;
    }
    return -1;
}

static int text_has(const struct klog_rec *r, const char *pfx)
{
    int i = 0;
    while (pfx[i]) {
        if (i >= (int)r->len || r->text[i] != pfx[i])
            return 0;
        i++;
    }
    return 1;
}

/* Walk every record committed since `from` and check the self-consistency of
 * the ones this probe wrote. Returns the number of torn records. */
static unsigned g_irq_cpumask;      /* cores that logged from interrupt context */

static unsigned verify_since(uint64_t from, unsigned *irq, unsigned *thr, unsigned *other)
{
    struct klog_rec r;
    unsigned torn = 0;
    *irq = *thr = *other = 0;
    g_irq_cpumask = 0;
    for (uint64_t s = from; s < klog_next_seq(); s++) {
        if (!klog_get(s, &r))
            continue;
        int is_irq = text_has(&r, "kdiag irq ");
        int is_thr = text_has(&r, "kdiag thr ");
        if (!is_irq && !is_thr) { (*other)++; continue; }
        long n = field_of(r.text, r.len, " n=");
        long t = field_of(r.text, r.len, " tail=");
        /* Both halves of a record's own counter must agree, AND the cpu the
         * record was committed under must be the cpu its text names. A line
         * assembled in a buffer shared with another core fails one or both. */
        long c = field_of(r.text, r.len, " cpu=");
        if (n < 0 || t < 0 || n != t || c != (long)r.cpu) { torn++; continue; }
        if (is_irq) { (*irq)++; g_irq_cpumask |= 1u << (r.cpu & 31); }
        else        { (*thr)++; }
    }
    return torn;
}

static unsigned popcount32(unsigned v)
{
    unsigned n = 0;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}

/* --- /dev/ktrigger -------------------------------------------------------- */

static void do_irqstorm(long rounds)
{
    if (rounds <= 0)   rounds = 400;
    if (rounds > 20000) rounds = 20000;

    if (!lapic_ready()) {
        klog(KL_ERR, "KDIAG_IRQSTORM skipped: LAPIC not up");
        return;
    }

    uint64_t from = klog_next_seq();
    uint64_t before = g_irq_total;
    int me = this_cpu()->index;
    int ncpu = smp_cpu_count();
    if (ncpu > PERCPU_MAXCPU) ncpu = PERCPU_MAXCPU;

    /* IF is 0 in here (int 0x80 is an interrupt gate), and an IPI cannot be
     * delivered to a core with interrupts masked -- so the storm has to run
     * with IF=1. That is safe for the same reason SYS_HTTP_GET's sti window
     * is: interrupts.c keys re-entrancy off g_bkl_owner, so a nested IRQ on a
     * core already holding the BKL neither re-acquires it nor re-enters the
     * scheduler. */
    __asm__ volatile ("sti");
    for (long i = 1; i <= rounds; i++) {
        klog(KL_DEBUG, "kdiag thr cpu=%d n=%lld tail=%lld", me, (long long)i, (long long)i);
        /* Self first: this is the one that can land INSIDE the klog call above,
         * which is the same-core case the interrupt guard exists for. */
        lapic_send_ipi((uint8_t)g_cpus[me].lapic_id, KDIAG_VEC_LOG);
        /* Then one remote core, rotating. Not all of them at once: a LAPIC has
         * a single IRR bit per vector, so a second IPI to a core that has not
         * serviced the first is COALESCED -- blasting every core each round
         * produces one delivery and looks like the others are not wired up. */
        if (ncpu > 1) {
            int c = (int)(i % ncpu);
            if (c != me)
                lapic_send_ipi((uint8_t)g_cpus[c].lapic_id, KDIAG_VEC_LOG);
        }
        for (volatile int s = 0; s < 3000; s++)
            ;                                /* let the remote core service it */
    }
    for (volatile int spin = 0; spin < 8000000; spin++)
        ;                                    /* drain the in-flight IPIs */
    __asm__ volatile ("cli");

    unsigned irq = 0, thr = 0, other = 0;
    unsigned torn = verify_since(from, &irq, &thr, &other);
    struct klog_stats st;
    klog_get_stats(&st);

    /* Deliberately at KL_ERR when it fails and KL_INFO when it passes, so the
     * verdict is on the console either way and the harness greps one line. */
    /* irq_hits is counted by the handler itself, so it is immune to records
     * ageing out of the ring; irq_cpus is derived from records that SURVIVED,
     * which is the number that says the ring holds interrupt-context lines. */
    unsigned hit_cpus = 0;
    for (int i = 0; i < ncpu; i++)
        if (g_irq_hits[i] > 0)
            hit_cpus++;

    klog(torn ? KL_ERR : KL_INFO,
         "KDIAG_IRQSTORM %s rounds=%lld irq_taken=%llu irq_hit_cpus=%u "
         "irq_records=%u irq_cpus=%u thr_records=%u torn=%u other=%u aged_out=%llu",
         torn ? "FAIL" : "ok", (long long)rounds,
         (unsigned long long)(g_irq_total - before), hit_cpus, irq,
         popcount32(g_irq_cpumask), thr, torn, other,
         (unsigned long long)st.overwritten);
}

static void do_logstorm(long n)
{
    if (n <= 0) n = 2000;
    if (n > 200000) n = 200000;
    uint64_t from = klog_next_seq();
    for (long i = 1; i <= n; i++)
        klog(KL_DEBUG, "kdiag thr cpu=%d n=%lld tail=%lld",
             this_cpu()->index, (long long)i, (long long)i);
    unsigned irq = 0, thr = 0, other = 0;
    unsigned torn = verify_since(from, &irq, &thr, &other);
    struct klog_stats st;
    klog_get_stats(&st);
    /* `retained` is deliberately less than `wrote` once the ring wraps: that IS
     * the defined full-ring behaviour, and printing both is how the harness
     * checks that overwriting happened without blocking. */
    klog(torn ? KL_ERR : KL_INFO,
         "KDIAG_LOGSTORM %s wrote=%lld retained=%u torn=%u aged_out=%llu capacity=%u",
         torn ? "FAIL" : "ok", (long long)n, thr, torn,
         (unsigned long long)st.overwritten, st.slots);
}

static long parse_long(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    long v = 0; int any = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    *pp = p;
    return any ? v : -1;
}

static int cmd_is(const char *b, const char *e, const char *word, const char **rest)
{
    const char *p = b;
    while (*word && p < e && *p == *word) { p++; word++; }
    if (*word)
        return 0;
    if (p < e && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\0')
        return 0;
    *rest = p;
    return 1;
}

int kdiag_write(const char *path, const void *buf, int len)
{
    if (!path || !streq(path, "/dev/ktrigger"))
        return KDIAG_NOT_MINE;
    if (len <= 0)
        return 0;

    const char *b = (const char *)buf;
    const char *e = b + len;
    while (b < e && (*b == ' ' || *b == '\n' || *b == '\t')) b++;
    const char *rest = b;

    if (cmd_is(b, e, "bt", &rest)) {
        klog(KL_INFO, "KDIAG_BT begin");
        backtrace_here(KL_INFO, 16);
        klog(KL_INFO, "KDIAG_BT end");
    } else if (cmd_is(b, e, "warn", &rest)) {
        WARN("deliberate WARN from /dev/ktrigger");
        klog(KL_INFO, "KDIAG_WARN done warns=%llu",
             (unsigned long long)panic_warn_count());
    } else if (cmd_is(b, e, "warn_on", &rest)) {
        int cond = 1;
        if (WARN_ON(cond))
            klog(KL_INFO, "KDIAG_WARN_ON continued warns=%llu",
                 (unsigned long long)panic_warn_count());
    } else if (cmd_is(b, e, "loglevel", &rest)) {
        long v = parse_long(&rest, e);
        klog_set_console_level((int)(v < 0 ? KL_INFO : v));
        klog(KL_INFO, "KDIAG_LOGLEVEL %d", klog_console_level());
    } else if (cmd_is(b, e, "logstorm", &rest)) {
        do_logstorm(parse_long(&rest, e));
    } else if (cmd_is(b, e, "irqstorm", &rest)) {
        do_irqstorm(parse_long(&rest, e));
    } else if (cmd_is(b, e, "assert", &rest)) {
        int zero = 0;
        KASSERT(zero == 1);          /* fires: panics with the expression text */
    } else if (cmd_is(b, e, "panic", &rest)) {
        while (rest < e && *rest == ' ') rest++;
        char msg[80];
        int i = 0;
        while (rest < e && *rest != '\n' && i < (int)sizeof msg - 1)
            msg[i++] = *rest++;
        msg[i] = 0;
        panic("deliberate panic from /dev/ktrigger: %s", i ? msg : "(no message)");
    } else {
        klog(KL_WARN, "KDIAG_TRIGGER unknown command "
             "(bt|warn|warn_on|loglevel N|logstorm N|irqstorm N|assert|panic MSG)");
    }
    return len;
}
