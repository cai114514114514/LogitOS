#ifndef LOGIT_KPROF_H
#define LOGIT_KPROF_H

#include <stdint.h>

/* ============================================================================
 * kprof -- the measurement instrument.
 *
 * WHY THIS EXISTS
 * ---------------
 * Every performance claim in this tree that was settled by an argument was
 * wrong, and every one settled by a measurement was right. Three examples, all
 * from this project:
 *
 *   - mini-libc's allocator was O(N^2). Nobody knew until it was measured:
 *     200,000 live objects went from "does not finish in 20 minutes" to 580 ms.
 *   - "emulation is why JavaScript is slow" was assumed. Measured, TCG costs
 *     8.5x on a tight integer loop while compile cost 609x and allocation
 *     3092x. The assumption was wrong by two orders of magnitude, and it was
 *     wrong about WHICH THING was slow, not just by how much.
 *   - a build failure was twice diagnosed as a stale build/ directory. It was a
 *     header collision.
 *
 * So this file is not a feature. It is the thing that makes the next five
 * "make it faster" changes cost a measurement instead of an argument.
 *
 * TWO INSTRUMENTS, BECAUSE THEY ANSWER DIFFERENT QUESTIONS
 * -------------------------------------------------------
 *   SAMPLING  asks "where is the machine, right now?", 1000 times a second, on
 *             every core, and answers "in this function". It needs no source
 *             changes, so it can find hot code nobody suspected -- which is the
 *             only way any of the three findings above could have been made.
 *
 *   SPANS     ask "how long did ONE page load spend in TLS versus layout versus
 *             paint?" -- a question about a named phase of one operation, which
 *             a flat address histogram cannot answer no matter how many samples
 *             it has. Spans need a source change and give exact totals.
 *
 * You want both. Sampling finds the function; spans apportion the wall clock.
 *
 * COST WHEN OFF
 * -------------
 * A profiler that slows the system it measures is a liar, so the disabled cost
 * is a designed property and a measured number, not a hope:
 *
 *   - SAMPLING off costs exactly nothing. The interrupt source is not armed; no
 *     instruction anywhere in the kernel is executed on its behalf.
 *   - SPANS off cost one load of a global and one never-taken branch per
 *     KPROF_BEGIN and per KPROF_END. Measured: see `echo overhead > /dev/kprof`.
 *   - -DKPROF_DISABLE removes even that: the macros expand to nothing, the
 *     module compiles to stubs, and /dev/kprof reports that it is not built in.
 *     That build is also the negative control -- tests/boot/run-prof-test.sh
 *     must FAIL against it.
 *
 * HOW TO USE IT
 * -------------
 * From a shell on the machine, with no debugger and no host tooling:
 *
 *     echo start   > /dev/kprof     # begin sampling every core
 *     ...do the thing you care about...
 *     echo stop    > /dev/kprof
 *     cat /dev/kprof                # the report
 *
 * From the host, with names instead of addresses:
 *
 *     python3 tools/ksymbolize.py --map build/kernel.map < report.txt
 *
 * From ring 3, for a phase you timed yourself:
 *
 *     echo "span tls 12345678" > /dev/kprof     # name, nanoseconds
 *
 * ==========================================================================*/

/* ---- limits ------------------------------------------------------------- */

#define KPROF_NSLOT      64      /* distinct named spans */
#define KPROF_NAMELEN    24
#define KPROF_HASH_BITS  13
#define KPROF_NBUCKET    (1 << KPROF_HASH_BITS)   /* 8192 kernel-RIP buckets */
#define KPROF_UBUCKET    1024                     /* ring-3 RIP buckets */
#define KPROF_PROBE      16      /* open-addressing probe limit */
#define KPROF_MAXCPU     8       /* == PERCPU_MAXCPU; kept private so kprof.c
                                  * compiles on the host without percpu.h */

/* The diagnostic IDT vector the sampler runs on. 238/239 are kdiag's, 240/241
 * are the TLB and present IPIs, 255 is LAPIC spurious, 0x60-0x7F are the
 * dynamic device vectors (irq.h), 128 is int 0x80, 32-47 the remapped PIC.
 * 237 is free. */
#define KPROF_VEC        237

/* ---- the runtime flags --------------------------------------------------
 * Read on the fast path of every span, so they are plain globals rather than
 * function calls: the disabled path must be a load and a predicted-not-taken
 * branch and nothing else. */
extern volatile int g_kprof_sampling;    /* the interrupt source is armed */
extern volatile int g_kprof_spans;       /* KPROF_BEGIN/END record */

/* ---- control ------------------------------------------------------------- */

void kprof_init(void);                   /* install the IDT gate (kdiag_init) */
int  kprof_start(uint32_t lapic_count);  /* 0 = default rate; returns 0 on success */
void kprof_stop(void);
void kprof_reset(void);
int  kprof_active(void);

/* Render the report. Returns bytes written. Safe to call at any time. */
int  kprof_report(char *buf, int max);
/* One line for /dev/kstat. */
int  kprof_summary(char *buf, int max);

/* /dev/kprof's write commands, factored out so kdiag.c stays a router.
 * Returns 0 if the command was understood. */
int  kprof_command(const char *b, const char *e);

/* ---- spans --------------------------------------------------------------- */

uint64_t kprof_span_begin_slow(int *slot, const char *name);
void     kprof_span_end_slow(int slot, uint64_t t0);

/* Intern a name once and return its slot, or -1. For code that cannot use the
 * lexical macros (a span that begins in one function and ends in another). */
int  kprof_slot(const char *name);
/* Post an already-measured duration. This is also what a ring-3 caller reaches
 * through `echo "span NAME NS" > /dev/kprof`. */
void kprof_span_add(int slot, uint64_t ns);

#ifndef KPROF_DISABLE

static inline uint64_t kprof_span_begin(int *slot, const char *name)
{
    /* THE DISABLED PATH. One load, one branch, hinted. Nothing else may appear
     * here -- not a clock read, not a call -- or the "zero cost when off" claim
     * stops being true and every number this profiler produces inherits the
     * error it introduced. */
    if (__builtin_expect(!g_kprof_spans, 1))
        return 0;
    return kprof_span_begin_slow(slot, name);
}

static inline void kprof_span_end(int slot, uint64_t t0)
{
    /* t0 == 0 means "begin ran while spans were off", so end is a no-op. This
     * also makes a BEGIN/END pair straddling a kprof_start() harmless: the span
     * is dropped rather than credited with time from before the profile began. */
    if (__builtin_expect(!t0, 1))
        return;
    kprof_span_end_slow(slot, t0);
}

/* The lexical form. The start timestamp lives in a LOCAL, not in per-CPU state:
 * a span may be preempted, migrated to another core, and finished there, and it
 * may nest inside another span. Both work, and neither needs a stack. */
#define KPROF_BEGIN(name)                                                   \
    static int _kprof_slot_##name = -1;                                     \
    uint64_t _kprof_t0_##name = kprof_span_begin(&_kprof_slot_##name, #name)

#define KPROF_END(name)                                                     \
    kprof_span_end(_kprof_slot_##name, _kprof_t0_##name)

#else  /* KPROF_DISABLE: the negative-control build.
        * The macros vanish entirely -- not even the flag test remains -- so a
        * build with this defined proves that the enabled path's cost is the
        * whole of the difference, and makes tests/boot/run-prof-test.sh fail,
        * which is what says that harness measures the profiler and not the
        * weather. */

#define KPROF_BEGIN(name)   ((void)0)
#define KPROF_END(name)     ((void)0)

#endif /* KPROF_DISABLE */

/* ---- what the tests read back -------------------------------------------- */

struct kprof_stats {
    uint64_t samples;         /* interrupts taken, counted per-CPU by the ISR */
    uint64_t kernel_samples;
    uint64_t user_samples;
    uint64_t recorded;        /* hits summed out of the shared accumulator */
    uint64_t overflow;        /* samples the hash could not place */
    uint64_t lost_ring;       /* samples with an unrecognisable CS */
    uint64_t run_ns;          /* monotonic ns the profile has been running */
    uint32_t cores;           /* bitmap of cores that produced a sample */
    uint32_t distinct;        /* buckets in use */
    uint64_t spans;           /* span END calls recorded */
};
void kprof_get_stats(struct kprof_stats *out);

/* Hits for one exact address, and for one span slot. The self-test uses these
 * to compare a distribution it already knows the answer to. */
uint64_t kprof_hits_in(uint64_t lo, uint64_t hi);
int      kprof_span_get(const char *name, uint64_t *count, uint64_t *total_ns,
                        uint64_t *max_ns);

#ifdef LOGIT_KPROF_HOST
/* Host-test entry points: the REAL accumulator, driven directly. A test of a
 * copy of the code proves things about the copy. */
void kprof_host_sample(uint64_t rip, int user, int cpu);
void kprof_host_reset(void);
#endif

#endif /* LOGIT_KPROF_H */
