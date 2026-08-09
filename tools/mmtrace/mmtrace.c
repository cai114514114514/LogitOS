/* mmtrace -- a QEMU TCG plugin that records LogitOS's user-page reference
 * string, exactly, so an offline simulator can compare this kernel's page
 * reclaim against Belady's MIN.
 *
 * ===========================================================================
 * WHY THIS AND NOT A KERNEL CHANGE
 *
 * The question being answered is "how far is c/kernel/mm/reclaim.c's clock
 * from the offline optimum", and the optimum is computable only from a
 * reference string. This machine cannot produce one from the inside. There is
 * no hardware reference notification -- that single sentence is what the whole
 * reclaim design in reclaim.h is built on -- so a kernel-resident tracer has
 * exactly two options and both damage the thing being measured:
 *
 *   1. FAULT ON ACCESS. Clear the present bit on every user page, log the
 *      minor fault, restore it. This is a real trace, and it is the standard
 *      technique. It also multiplies the cost of a first touch in every window
 *      by a page fault, which changes WHEN the kernel hits its watermarks, and
 *      therefore changes the very reclaim behaviour under measurement. It also
 *      needs a new PTE state living alongside the swap encoding in vmm.h, in a
 *      kernel where eleven other harnesses boot the same image.
 *
 *   2. ACCESSED-BIT SAMPLING. Cheap, already implemented (it IS the clock's
 *      sweep), and it cannot see order within a sweep. Belady needs order.
 *      It would yield a bound, not the gap.
 *
 * The emulator is outside the system under test and sees every access with no
 * ambiguity at all. The guest kernel is not modified by one byte; the trace is
 * produced only when `-plugin` is passed, so the eleven other harnesses boot
 * the identical image. That is the whole argument for doing it here.
 *
 * ===========================================================================
 * WHAT IS CAPTURED, AND WHAT IS DELIBERATELY NOT
 *
 * Filtered to the private user region [0x40000000, 0x80000000) -- MM_USER_BASE
 * to MM_USER_END in c/kernel/mm/mm.h. That range is exactly the set of virtual
 * addresses reclaim is allowed to touch (mm_fault_classify() refuses every
 * fault outside it), and the kernel's own identity map sits BELOW it, so a
 * one-comparison address filter separates "a page reclaim manages" from
 * "kernel memory" with no register reads and no heuristics.
 *
 * A kernel access to a user page through usercopy uses the user virtual
 * address and is therefore captured -- correctly, because it is a genuine
 * reference to that page and a policy that evicts it is genuinely wrong. A
 * kernel access through the identity map (do_anon's memset, reclaim's own
 * page_is_zero, the swap write) is NOT captured -- also correctly: those are
 * the mechanism, not the workload, and counting them would let the tracer's
 * subject reference its own pages.
 *
 * INSTRUCTION FETCHES ARE CAPTURED (kind=X). They must be: the CPU sets the
 * accessed bit on a fetch, so the clock sees text pages as referenced, and a
 * simulator fed only data accesses would evict the page a process is running
 * out of and report a fault the real machine never takes.
 *
 * ===========================================================================
 * COLLAPSING, AND THE ONE THING IT MUST NOT DESTROY
 *
 * Consecutive references to one page are collapsed to a single record. Every
 * stack-based policy gives the identical answer either way, and it is the
 * difference between a 200 MB trace and a 20 GB one. The window is ONE, so
 * A,B,A,B is four records and not two -- a wider window would fold recency
 * order away, and recency order is the only thing LRU and MIN are computed
 * from. The collapse is per-vCPU and is broken whenever CR3 might have
 * changed, so a context switch can never be hidden inside a run.
 *
 * A run remembers whether ANY access in it was a store, so a page that is only
 * ever read is distinguishable from one that is written -- which is what tells
 * the offline analysis which evictions could have been the free tier-1 drop
 * and which had to go to the disk.
 *
 * ===========================================================================
 * ARGUMENTS
 *
 *   out=<path>     trace file (required)
 *   lo=0x40000000  low end of the traced virtual window
 *   hi=0x80000000  high end (exclusive)
 *   exec=on|off    trace instruction fetches (default on)
 *   limit=<n>      stop after n records (default 0 = unlimited)
 *
 * Build and use: see tests/mmtrace.mk. */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <qemu-plugin.h>

#include "mmtrace_fmt.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_VCPU  16
#define BUFREC    (1u << 16)      /* 64 Ki records = 1 MiB per vCPU */

struct vcpu_st {
    /* the run currently being collapsed; not yet in the buffer */
    bool     pend_valid;
    uint64_t pend_vpn;
    unsigned pend_kind;
    uint32_t pend_pfn;
    uint32_t pend_space;
    unsigned pend_cpu;

    bool     need_cr3;            /* a mov-to-CR3 executed; re-read before use */
    uint32_t space;               /* cached CR3 >> 12 */

    struct qemu_plugin_register *cr3_reg;
    GByteArray *regbuf;

    unsigned n;
    struct mmt_rec buf[BUFREC];
} __attribute__((aligned(64)));

static struct vcpu_st g_cpu[MAX_VCPU];

static FILE          *g_out;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t       g_nrec;
static uint64_t       g_limit;
static uint64_t       g_lo = 0x40000000ull, g_hi = 0x80000000ull;
static bool           g_exec = true;
static bool           g_stopped;
static bool           g_have_cr3;
static uint64_t       g_dropped;   /* records lost after the limit was hit */

/* ------------------------------------------------------------- output --- */

static void flush_locked(struct vcpu_st *v)
{
    if (!v->n) return;
    if (g_out && !g_stopped)
        fwrite(v->buf, sizeof(struct mmt_rec), v->n, g_out);
    g_nrec += v->n;
    v->n = 0;
    if (g_limit && g_nrec >= g_limit) g_stopped = true;
}

static void push(struct vcpu_st *v, uint64_t a, uint64_t b)
{
    if (g_stopped) { g_dropped++; return; }
    v->buf[v->n].a = a;
    v->buf[v->n].b = b;
    if (++v->n == BUFREC) {
        pthread_mutex_lock(&g_lock);
        flush_locked(v);
        pthread_mutex_unlock(&g_lock);
    }
}

static void close_run(struct vcpu_st *v)
{
    if (!v->pend_valid) return;
    push(v, MMT_MK_A(v->pend_vpn, v->pend_kind, v->pend_cpu),
            MMT_MK_B(v->pend_space, v->pend_pfn));
    v->pend_valid = false;
}

/* ---------------------------------------------------------------- CR3 --- */

static uint32_t read_space(struct vcpu_st *v)
{
    if (!v->cr3_reg || !v->regbuf) return 0;
    g_byte_array_set_size(v->regbuf, 0);
    int n = qemu_plugin_read_register(v->cr3_reg, v->regbuf);
    if (n <= 0) return 0;
    uint64_t cr3 = 0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++)                     /* target byte order = LE */
        cr3 |= (uint64_t)v->regbuf->data[i] << (8 * i);
    return (uint32_t)((cr3 >> 12) & 0xFFFFFFull);
}

/* ------------------------------------------------------------ the emit --- */

static void reference(unsigned cpu, uint64_t vaddr, unsigned kind, uint32_t pfn)
{
    if (cpu >= MAX_VCPU) return;
    if (vaddr - g_lo >= g_hi - g_lo) return;        /* one compare, both ends */
    if (g_stopped) return;

    struct vcpu_st *v = &g_cpu[cpu];
    uint64_t vpn = vaddr >> 12;

    /* Same page as the run in progress, and no CR3 write has intervened:
     * fold it in. A store anywhere in the run upgrades the run's kind, so a
     * page that was written is never recorded as read-only. */
    if (v->pend_valid && !v->need_cr3 && v->pend_vpn == vpn) {
        if (kind == MMT_KIND_WRITE && v->pend_kind == MMT_KIND_READ)
            v->pend_kind = MMT_KIND_WRITE;
        if (!v->pend_pfn && pfn) v->pend_pfn = pfn;
        return;
    }

    if (v->need_cr3) { v->space = read_space(v); v->need_cr3 = false; }

    close_run(v);
    v->pend_valid = true;
    v->pend_vpn   = vpn;
    v->pend_kind  = kind;
    v->pend_pfn   = pfn;
    v->pend_space = v->space;
    v->pend_cpu   = cpu;
}

/* --------------------------------------------------------- callbacks --- */

static void cb_mem(unsigned int cpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *ud)
{
    (void)ud;
    uint32_t pfn = 0;
    struct qemu_plugin_hwaddr *h = qemu_plugin_get_hwaddr(info, vaddr);
    if (h && !qemu_plugin_hwaddr_is_io(h))
        pfn = (uint32_t)((qemu_plugin_hwaddr_phys_addr(h) >> 12) & 0xFFFFFFull);
    reference(cpu, vaddr,
              qemu_plugin_mem_is_store(info) ? MMT_KIND_WRITE : MMT_KIND_READ,
              pfn);
}

static void cb_fetch(unsigned int cpu, void *ud)
{
    reference(cpu, (uint64_t)(uintptr_t)ud, MMT_KIND_EXEC, 0);
}

/* A write to CR3 is the only way this kernel changes address space (there is
 * no task switch and no VMX here), so marking the vCPU "CR3 unknown" here and
 * re-reading it lazily at the next reference costs one register read per
 * CONTEXT SWITCH instead of one per memory access. Reading it eagerly on every
 * access was the first version and made the tracer the slowest thing in the
 * system by an order of magnitude.
 *
 * The callback fires BEFORE the instruction executes, which is exactly right:
 * the re-read happens at the next reference, by which time the new CR3 is
 * live. A `mov %cr3,%rax` (a read) also matches and merely costs one redundant
 * re-read. */
static void cb_cr3_touch(unsigned int cpu, void *ud)
{
    (void)ud;
    if (cpu < MAX_VCPU) g_cpu[cpu].need_cr3 = true;
}

static void cb_tb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    size_t n = qemu_plugin_tb_n_insns(tb);
    uint64_t last_page = ~0ull;

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);

        qemu_plugin_register_vcpu_mem_cb(insn, cb_mem, QEMU_PLUGIN_CB_R_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);

        if (g_exec && va - g_lo < g_hi - g_lo) {
            /* One fetch record per page the block runs through, not one per
             * instruction: within a page the references are to the same page
             * and would all collapse anyway, so this is the same trace for a
             * fraction of the callbacks. */
            uint64_t page = va >> 12;
            if (page != last_page) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, cb_fetch, QEMU_PLUGIN_CB_R_REGS,
                    (void *)(uintptr_t)va);
                last_page = page;
            }
        }

        char *d = qemu_plugin_insn_disas(insn);
        if (d) {
            if (strstr(d, "cr3"))
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, cb_cr3_touch, QEMU_PLUGIN_CB_NO_REGS, NULL);
            g_free(d);
        }
    }
}

static void cb_vcpu_init(qemu_plugin_id_t id, unsigned int cpu)
{
    (void)id;
    if (cpu >= MAX_VCPU) return;
    struct vcpu_st *v = &g_cpu[cpu];
    v->need_cr3 = true;
    if (!v->regbuf) v->regbuf = g_byte_array_new();
    if (v->cr3_reg) return;

    GArray *regs = qemu_plugin_get_registers();
    if (!regs) return;
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (d->name && strcmp(d->name, "cr3") == 0) { v->cr3_reg = d->handle; break; }
    }
    g_array_free(regs, true);

    if (cpu == 0) {
        g_have_cr3 = v->cr3_reg != NULL;
        qemu_plugin_outs(g_have_cr3
            ? "mmtrace: CR3 is readable; records carry a real address-space id\n"
            : "mmtrace: WARNING no cr3 register exposed -- space ids will be 0, "
              "so a multi-process trace cannot be split\n");
    }
}

static void cb_exit(qemu_plugin_id_t id, void *p)
{
    (void)id; (void)p;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_VCPU; i++) {
        /* close_run() may push, and push() takes the lock on a full buffer.
         * The buffers are drained here one at a time with room guaranteed
         * (flush first, then close, then flush), so that path is not taken. */
        flush_locked(&g_cpu[i]);
        if (g_cpu[i].pend_valid) {
            struct vcpu_st *v = &g_cpu[i];
            v->buf[v->n].a = MMT_MK_A(v->pend_vpn, v->pend_kind, v->pend_cpu);
            v->buf[v->n].b = MMT_MK_B(v->pend_space, v->pend_pfn);
            v->n++;
            v->pend_valid = false;
        }
        flush_locked(&g_cpu[i]);
    }
    if (g_out) {
        struct mmt_hdr h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, MMT_MAGIC, 8);
        h.version = MMT_VERSION;
        h.recsize = (uint32_t)sizeof(struct mmt_rec);
        h.va_lo = g_lo; h.va_hi = g_hi;
        h.flags = (g_exec ? MMT_F_EXEC : 0) | (g_have_cr3 ? MMT_F_CR3 : 0);
        h.nrec  = g_nrec;
        fseek(g_out, 0, SEEK_SET);
        fwrite(&h, sizeof h, 1, g_out);
        fclose(g_out);
        g_out = NULL;
    }
    pthread_mutex_unlock(&g_lock);

    char msg[256];
    snprintf(msg, sizeof msg,
             "mmtrace: %" PRIu64 " page references written%s\n",
             g_nrec, g_dropped ? " (LIMIT HIT -- trace is truncated)" : "");
    qemu_plugin_outs(msg);
}

/* ---------------------------------------------------------------- setup --- */

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    (void)info;
    const char *path = NULL;

    for (int i = 0; i < argc; i++) {
        char *a = argv[i];
        if (!strncmp(a, "out=", 4))        path = a + 4;
        else if (!strncmp(a, "lo=", 3))    g_lo = strtoull(a + 3, NULL, 0);
        else if (!strncmp(a, "hi=", 3))    g_hi = strtoull(a + 3, NULL, 0);
        else if (!strncmp(a, "limit=", 6)) g_limit = strtoull(a + 6, NULL, 0);
        else if (!strncmp(a, "exec=", 5))  g_exec = strcmp(a + 5, "off") != 0;
        else { fprintf(stderr, "mmtrace: unknown argument '%s'\n", a); return -1; }
    }
    if (!path) { fprintf(stderr, "mmtrace: out=<path> is required\n"); return -1; }
    if (g_hi <= g_lo) { fprintf(stderr, "mmtrace: hi must exceed lo\n"); return -1; }

    g_out = fopen(path, "wb");
    if (!g_out) { perror("mmtrace: open trace"); return -1; }
    setvbuf(g_out, NULL, _IOFBF, 8u << 20);

    struct mmt_hdr h;                       /* placeholder; rewritten at exit */
    memset(&h, 0, sizeof h);
    fwrite(&h, sizeof h, 1, g_out);

    qemu_plugin_register_vcpu_init_cb(id, cb_vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, cb_tb);
    qemu_plugin_register_atexit_cb(id, cb_exit, NULL);
    return 0;
}
