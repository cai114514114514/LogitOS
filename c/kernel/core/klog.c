/* klog -- the kernel log ring. See klog.h for the safety argument; this file
 * is the mechanism. Nothing in here allocates, blocks, or takes a lock other
 * than its own leaf lock. */

#include <stdint.h>
#include <stdarg.h>
#include "klog.h"
#include "kprintf.h"
#include "vga.h"
#include "serial.h"
#include "spinlock.h"
#include "percpu.h"
#include "pit.h"
/* Angle brackets on purpose, against the house style. A quoted include is
 * searched in the INCLUDING FILE'S directory first, which would always find
 * c/kernel/core/kirq.h and make it impossible for the host test to shadow it
 * with tests/unit/klogstub/kirq.h -- and `cli` is privileged, so a ring-3 test
 * process cannot execute the real one. `<>` skips that rule and honours the
 * -I order, which is exactly what the shadowing depends on. */
#include <kirq.h>

/* --- the two guards, and what each one excludes ---------------------------
 * irq_save/irq_restore make the PER-CPU line buffer safe against an interrupt
 * handler landing on THIS core mid-append. It is not a lock: the buffer is
 * per-CPU, so the only racer is this core's own IRQ, and IF=0 excludes it.
 * Cross-core exclusion is g_klog_lock's job, and only the commit takes that.
 *
 * -DKLOG_UNSAFE removes both -- one shared line buffer, no interrupt guard,
 * i.e. the design you get if you do not think about interrupt context. It is
 * the negative control: with it, the interleaving test in tests/unit/log_test.c
 * fails and `echo irqstorm > /dev/ktrigger` reports torn records. */
static inline uint64_t irq_save(void)
{
#ifdef KLOG_UNSAFE
    return 0;
#else
    return kirq_save();
#endif
}
static inline void irq_restore(uint64_t f)
{
#ifdef KLOG_UNSAFE
    (void)f;
#else
    kirq_restore(f);
#endif
}

/* --- the ring ------------------------------------------------------------- */

static struct klog_rec g_ring[KLOG_SLOTS];
static spinlock_t      g_klog_lock = SPINLOCK_INIT;
static volatile uint64_t g_seq = 1;          /* seq the next record will take */
static volatile uint64_t g_truncated_lines;
static volatile uint64_t g_truncated_chars;
static volatile uint64_t g_by_level[KL_NLEVELS];
static volatile int      g_console_level = KL_INFO;

/* Set while the ring is being dumped to the console, so the dump's own output
 * does not feed back into the ring it is walking. */
static volatile int g_dumping;
/* Set by klog_panic_takeover(): commit without the lock, because the core that
 * held it may be the one that died. */
static volatile int g_lockless;

/* Per-CPU line assembly. A record is one console LINE; kprintf hands us
 * characters, and a line is committed when its '\n' arrives (or when the panic
 * path asks for the partial tail). Keeping this per-CPU is what lets the slow
 * console write run with no lock held. */
struct klog_line {
    uint16_t len;
    uint32_t dropped;
    uint8_t  level;
    char     t[KLOG_TEXT];
};
static struct klog_line g_line[PERCPU_MAXCPU];

static int cpu_index(void)
{
    int i = this_cpu()->index;
    if (i < 0 || i >= PERCPU_MAXCPU)
        i = 0;
#ifdef KLOG_UNSAFE
    i = 0;      /* negative control: ONE shared line buffer for every core */
#endif
    return i;
}

/* Commit one assembled line. Caller must already have IF=0. */
static void klog_commit(struct klog_line *L, int cpu)
{
    uint64_t fl = 0;
    if (!g_lockless)
        fl = spin_lock_irqsave(&g_klog_lock);

    uint64_t s = g_seq++;
    struct klog_rec *r = &g_ring[(unsigned)(s & (KLOG_SLOTS - 1))];

    /* seq=0 first, seq=s last: a reader that races a lockless (panic-mode)
     * commit sees "empty" rather than half of two different lines. */
    r->seq = 0;
    __asm__ volatile ("" ::: "memory");
    r->ms      = timer_ms();
    r->dropped = L->dropped;
    r->len     = L->len;
    r->level   = L->level;
    r->cpu     = (uint8_t)cpu;
    for (int i = 0; i < (int)L->len; i++)
        r->text[i] = L->t[i];
    __asm__ volatile ("" ::: "memory");
    r->seq = s;

    if (L->level < KL_NLEVELS)
        g_by_level[L->level]++;
    if (L->dropped) {
        g_truncated_lines++;
        g_truncated_chars += L->dropped;
    }

    if (!g_lockless)
        spin_unlock_irqrestore(&g_klog_lock, fl);
}

static void line_reset(struct klog_line *L)
{
    L->len = 0;
    L->dropped = 0;
    L->level = KL_INFO;
}

void klog_putc(int level, char c)
{
    if (g_dumping)
        return;

    uint64_t f = irq_save();
    int cpu = cpu_index();
    struct klog_line *L = &g_line[cpu];

    /* A line's severity: set by its first character, then lowered (= made more
     * severe) by any later one. The "first character" case is not cosmetic --
     * g_line lives in .bss, so without it every line would start life at
     * level 0 (KL_PANIC) and the min() rule would keep it there forever. */
    if (level >= 0 && level < KL_NLEVELS) {
        if (L->len == 0 && L->dropped == 0)
            L->level = (uint8_t)level;
        else if ((uint8_t)level < L->level)
            L->level = (uint8_t)level;
    }

    if (c == '\n') {
        if (L->len || L->dropped)
            klog_commit(L, cpu);
        line_reset(L);
    } else if (c == '\r') {
        /* swallow: serial carriage returns are transport, not content */
    } else if (L->len < KLOG_TEXT) {
        L->t[L->len++] = c;
    } else {
        L->dropped++;                    /* defined behaviour: TRUNCATE */
    }
    irq_restore(f);
}

void klog_flush_partial(void)
{
    uint64_t f = irq_save();
    for (int i = 0; i < PERCPU_MAXCPU; i++) {
        struct klog_line *L = &g_line[i];
        if (L->len || L->dropped) {
            klog_commit(L, i);
            line_reset(L);
        }
    }
    irq_restore(f);
}

/* --- levelled entry points ------------------------------------------------ */

int  klog_console_level(void) { return g_console_level; }

void klog_set_console_level(int level)
{
    if (level < KL_PANIC) level = KL_PANIC;
    if (level > KL_DEBUG) level = KL_DEBUG;
    g_console_level = level;
}

void klogv(int level, const char *fmt, va_list ap)
{
    /* KL_PANIC prints bare: the panic report is a block with its own banner,
     * and prefixing every register line with "[panic]" would only make it
     * harder to read. Severity is preserved in the record either way. */
    static const char *const tag[KL_NLEVELS] = {
        "", "[err] ", "[warn] ", "", "",
    };
    int console = (level <= g_console_level);
    const char *t = (level >= 0 && level < KL_NLEVELS) ? tag[level] : "";
    kvlog_out(level, console, t, fmt, ap);
}

void klog(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klogv(level, fmt, ap);
    va_end(ap);
}

/* --- consumers ------------------------------------------------------------ */

uint64_t klog_next_seq(void) { return g_seq; }

uint64_t klog_first_seq(void)
{
    uint64_t n = g_seq;
    return (n > KLOG_SLOTS) ? (n - KLOG_SLOTS) : 1;
}

int klog_get(uint64_t seq, struct klog_rec *out)
{
    if (seq == 0 || seq >= g_seq)
        return 0;
    uint64_t fl = g_lockless ? 0 : spin_lock_irqsave(&g_klog_lock);
    struct klog_rec *r = &g_ring[(unsigned)(seq & (KLOG_SLOTS - 1))];
    int ok = (r->seq == seq);
    if (ok)
        *out = *r;
    if (!g_lockless)
        spin_unlock_irqrestore(&g_klog_lock, fl);
    return ok;
}

void klog_get_stats(struct klog_stats *out)
{
    uint64_t n = g_seq - 1;
    out->records         = n;
    out->overwritten     = (n > KLOG_SLOTS) ? (n - KLOG_SLOTS) : 0;
    out->truncated_lines = g_truncated_lines;
    out->truncated_chars = g_truncated_chars;
    for (int i = 0; i < KL_NLEVELS; i++)
        out->by_level[i] = g_by_level[i];
    out->slots    = KLOG_SLOTS;
    out->text_max = KLOG_TEXT;
}

int klog_format_rec(const struct klog_rec *r, char *buf, int max)
{
    static const char lch[KL_NLEVELS] = { 'P', 'E', 'W', 'I', 'D' };
    char l = (r->level < KL_NLEVELS) ? lch[r->level] : '?';
    int n = ksnprintf(buf, max, "[%5u.%03u] %c cpu%u ",
                      (unsigned)(r->ms / 1000), (unsigned)(r->ms % 1000),
                      l, (unsigned)r->cpu);
    for (int i = 0; i < (int)r->len && n < max - 1; i++)
        buf[n++] = r->text[i];
    if (r->dropped && n < max - 1)
        n += ksnprintf(buf + n, max - n, " <+%u truncated>", (unsigned)r->dropped);
    if (n < max - 1)
        buf[n++] = '\n';
    if (n < max)
        buf[n] = 0;
    return n;
}

int klog_render(char *buf, int max)
{
    struct klog_rec rec;
    int n = 0;
    for (uint64_t s = klog_first_seq(); s < klog_next_seq(); s++) {
        if (max - n < 8)
            break;
        if (!klog_get(s, &rec))
            continue;                    /* aged out while we walked */
        n += klog_format_rec(&rec, buf + n, max - n);
    }
    if (n < max)
        buf[n] = 0;
    return n;
}

void klog_dump_tail(int lines)
{
    klog_dump_tail_before(g_seq, lines);
}

void klog_dump_tail_before(uint64_t before_seq, int lines)
{
    uint64_t last = before_seq;
    uint64_t first = klog_first_seq();
    if (last > g_seq) last = g_seq;
    if (lines > 0 && last > (uint64_t)lines && last - (uint64_t)lines > first)
        first = last - (uint64_t)lines;

    g_dumping = 1;                        /* stop the dump feeding the ring */
    for (uint64_t s = first; s < last; s++) {
        struct klog_rec *r = &g_ring[(unsigned)(s & (KLOG_SLOTS - 1))];
        if (r->seq != s)
            continue;
        char line[KLOG_TEXT + 64];
        klog_format_rec(r, line, sizeof line);
        for (int i = 0; line[i]; i++) {
            vga_putc(line[i]);
            serial_putc(line[i]);
        }
    }
    g_dumping = 0;
}

void klog_panic_takeover(void)
{
    /* The dying core may BE the lock holder. Re-initialise it and stop taking
     * it at all: a garbled line beats a report that never prints. */
    g_klog_lock.ticket = 0;
    g_klog_lock.serving = 0;
    g_lockless = 1;
}
