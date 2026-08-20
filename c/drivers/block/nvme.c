/* From-scratch NVMe block driver (M24 bare-metal: the target box boots off NVMe).
 *
 * Minimal polled driver: one admin queue + one I/O queue (depth 64), Identify
 * Namespace for capacity/LBA size, and Read/Write via a single page-aligned DMA
 * bounce buffer (so PRP1 alone covers every <=4 KiB transfer -- no PRP2/PRP-list
 * and no caller-alignment worries). Structurally mirrors virtio.c/virtio_blk.c:
 * find the PCI device, map BAR0 MMIO, build the queues in identity-mapped DMA
 * frames, submit + POLL completions with interrupts on but non-preemptible
 * (g_nvme_busy, see interrupts.c). Slots into blkdev.c as the preferred backend.
 *
 * Validated under QEMU `-device nvme` (vendor 1b36:0010, PCI class 0x010802); the
 * class scan also finds a real controller on bare metal.
 */
#include <stdint.h>
#include <stddef.h>
#include "nvme.h"
#include "blkdev.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "pit.h"
#include "percpu.h"
#include "kprintf.h"

void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);

static inline uint32_t r32(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static inline uint64_t r64(volatile uint8_t *b, int o) { return *(volatile uint64_t *)(b + o); }
static inline void w32(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void w64(volatile uint8_t *b, int o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4, (uint32_t)(v >> 32)); }
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

/* BAR0 register offsets */
#define REG_CAP   0x00   /* u64 */
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28   /* u64 */
#define REG_ACQ   0x30   /* u64 */

#define AQ_DEPTH  64
#define IO_DEPTH  64

/* ONE SUBMISSION/COMPLETION PAIR PER CORE.
 *
 * NVMe is designed for this -- the doorbell for queue n is at a distinct
 * address, so two cores submitting at once touch two registers rather than
 * queueing behind one -- and this tree has already been here once: the kheap
 * magazines took `make test-smp` from 30.7 MILLION acquisitions of one lock to
 * about 112 by giving each core its own structure (CLAUDE.md, "The BKL").
 *
 * SAY WHAT IT BUYS TODAY, WHICH IS NOTHING MEASURABLE, because a capability
 * with no consumer is worth less than an honest note about it. Every path into
 * this driver runs under the BKL, so there is never a second submitter to
 * contend with, and the block layer above allows one request in flight per
 * medium -- so on this machine the per-core queues are exercised (a request
 * submitted from core 2 really does go to queue 2, which is what stops the
 * code from rotting) but they cannot make anything faster. What would make
 * them pay is a block path outside the BKL, which is the project
 * docs/superpowers/specs/2026-08-17-bkl-removal.md describes and not this one.
 *
 * The count is bounded by NVME_IOQ_MAX rather than by PERCPU_MAXCPU: each pair
 * costs two 4 KiB frames from the PMM at boot, and this machine is tested at
 * -smp 4. A core with an index past the last queue uses queue 0 -- correct,
 * just not private. */
#define NVME_IOQ_MAX 4

struct nvme_sqe {                 /* 64-byte submission queue entry */
    uint32_t cdw0; uint32_t nsid;
    uint64_t rsvd; uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

struct nvme_cqe {                 /* 16-byte completion queue entry */
    uint32_t result, rsvd;
    uint16_t sqhd, sqid, cid, status;   /* status: bit0 = phase, bits15:1 = status code */
} __attribute__((packed));

struct nvme_q {
    struct nvme_sqe *sq;          /* page-aligned DMA */
    struct nvme_cqe *cq;
    volatile uint8_t *sq_db, *cq_db;
    uint16_t sq_tail, cq_head, depth, cid;
    uint8_t  cq_phase;
};

static volatile uint8_t *g_regs;
static struct nvme_q g_admin, g_io[NVME_IOQ_MAX];
static int      g_nioq = 1;            /* I/O queue pairs actually created */
static uint32_t g_nsid = 1, g_lba = 512;
static uint64_t g_cap;                 /* capacity in LBAs */
static uint64_t *g_prp_list[NVME_IOQ_MAX];  /* one PRP-list page PER QUEUE: two queues in
                                             * flight at once would otherwise overwrite
                                             * each other's page list mid-DMA */
static uint32_t g_max_sectors = 0xFFFF;/* per-command cap from the controller's MDTS */
static int g_ready = 0;
static struct nvme_health g_health;    /* last SMART/health read; see nvme_health() */
static int g_health_ok = 0;

volatile int g_nvme_busy = 0;          /* interrupts.c: don't preempt mid-poll */
int nvme_busy(void)    { return g_nvme_busy; }
int nvme_present(void) { return g_ready; }
uint64_t nvme_capacity(void) { return g_ready ? g_cap : 0; }

static uint64_t map_bar0(uint8_t slot)
{
    uint32_t lo = pci_cfg_read(0, slot, 0, 0x10);
    if (lo == 0 || lo == 0xFFFFFFFF || (lo & 0x1)) return 0;   /* absent or I/O BAR */
    uint64_t base = lo & ~(uint64_t)0xF;
    if ((lo & 0x6) == 0x4)             /* 64-bit BAR: high dword in the next slot */
        base |= (uint64_t)pci_cfg_read(0, slot, 0, 0x14) << 32;
    if (!base) return 0;
    vmm_map_range(base, base, 0x8000, VMM_WRITABLE | VMM_NOCACHE);
    return base;
}

/* Find an NVMe controller by PCI class 0x010802 (NVM Express I/O controller) --
 * matches both QEMU's 1b36:0010 and a real SSD. Enables MEM space + bus master. */
static int nvme_find(struct pci_dev *out)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_cfg_read(0, slot, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cls = pci_cfg_read(0, slot, 0, 0x08);
        if ((cls >> 8) != 0x010802) continue;
        out->bus = 0; out->slot = slot; out->func = 0;
        out->vendor = id & 0xFFFF; out->device = id >> 16;
        out->bar0 = pci_cfg_read(0, slot, 0, 0x10) & ~(uint32_t)0xF;
        uint32_t cmd = pci_cfg_read(0, slot, 0, 0x04);
        pci_cfg_write(0, slot, 0, 0x04, cmd | 0x02 | 0x04);   /* MEM space + bus master */
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * SUBMIT AND POLL, SPLIT
 *
 * This used to be one function that wrote the SQE, rang the doorbell and then
 * spun on the CQ until the phase bit flipped -- and that spin is precisely the
 * BKL-held time c/kernel/mm/swap.c measures and blkdev.h now exists to give
 * back. The two halves are separate calls; nvme_run() below re-composes them
 * for the callers that genuinely have nowhere to go (controller bring-up, with
 * IF=0 and no scheduler yet).
 * ------------------------------------------------------------------------- */

/* Write one SQE and ring the tail doorbell. Returns the command id. Does NOT
 * touch IF or the no-preempt flag: whoever is going to wait decides that, and
 * for the block path the answer is blkdev.c (synchronous) or nobody (async). */
static uint16_t nvme_begin(struct nvme_q *q, const struct nvme_sqe *cmd)
{
    uint16_t cid = q->cid++;
    struct nvme_sqe *slot = &q->sq[q->sq_tail];
    memset(slot, 0, sizeof *slot);
    *slot = *cmd;
    slot->cdw0 = (slot->cdw0 & 0xFFFF) | ((uint32_t)cid << 16);
    barrier();
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    *(volatile uint32_t *)q->sq_db = q->sq_tail;
    return cid;
}

/* Consume at most ONE completion. 1 = the command with `cid` finished and
 * *status holds its status code; 0 = nothing for us yet.
 *
 * At most one, not "drain": a poll that looped would be a spin again, in the
 * one function whose whole purpose is not to be. A stale CQE from a command
 * that timed out is still retired here (head advanced, doorbell rung) and
 * reported as "not ours", exactly as the old loop's `continue` did -- dropping
 * it instead would leave the phase bit permanently disagreeing. */
static int nvme_step(struct nvme_q *q, uint16_t cid, int *status)
{
    barrier();
    volatile struct nvme_cqe *e = &q->cq[q->cq_head];
    if ((e->status & 1) != q->cq_phase) return 0;

    uint16_t ecid  = e->cid;
    uint16_t estat = (uint16_t)((e->status >> 1) & 0x7FFF);
    q->cq_head = (uint16_t)((q->cq_head + 1) % q->depth);
    if (q->cq_head == 0) q->cq_phase ^= 1;
    *(volatile uint32_t *)q->cq_db = q->cq_head;
    if (ecid != cid) return 0;
    *status = estat;
    return 1;
}

/* Begin + step to completion, for the bring-up path only.
 *
 * Kept as a spin COUNT rather than a millisecond deadline on purpose: this
 * runs from kmain before the scheduler exists and, at the point of the very
 * first admin command, with the PIT not yet advancing -- a ms deadline there
 * never fires. Returns the status code (0 = success, <0 = timeout). */
static int nvme_run(struct nvme_q *q, const struct nvme_sqe *cmd)
{
    uint64_t fl; __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    g_nvme_busy++;
    __asm__ volatile ("sti");
    uint16_t cid = nvme_begin(q, cmd);

    int status = -1;
    for (long spins = 0; spins < 200000000L; spins++)
        if (nvme_step(q, cid, &status)) break;

    if (!(fl & 0x200)) __asm__ volatile ("cli");
    g_nvme_busy--;
    return status;
}

/* Free every queue/DMA frame allocated so far (init failure unwind). The
 * controller is disabled first so it can't DMA into frames returned to the PMM. */
static void nvme_free_all(void)
{
    w32(g_regs, REG_CC, 0);
    if (g_admin.sq) { pmm_free((uint64_t)(uintptr_t)g_admin.sq); g_admin.sq = NULL; }
    if (g_admin.cq) { pmm_free((uint64_t)(uintptr_t)g_admin.cq); g_admin.cq = NULL; }
    for (int i = 0; i < NVME_IOQ_MAX; i++) {
        if (g_io[i].sq)    { pmm_free((uint64_t)(uintptr_t)g_io[i].sq);    g_io[i].sq = NULL; }
        if (g_io[i].cq)    { pmm_free((uint64_t)(uintptr_t)g_io[i].cq);    g_io[i].cq = NULL; }
        if (g_prp_list[i]) { pmm_free((uint64_t)(uintptr_t)g_prp_list[i]); g_prp_list[i] = NULL; }
    }
    g_nioq = 1;
}

int nvme_init(void)
{
    struct pci_dev dev;
    if (nvme_find(&dev) != 0) return -1;
    uint64_t bar0 = map_bar0(dev.slot);
    if (!bar0) { kprintf("[nvme] invalid BAR0\n"); return -1; }
    g_regs = (volatile uint8_t *)bar0;

    uint64_t cap = r64(g_regs, REG_CAP);
    uint32_t stride = 4u << ((cap >> 32) & 0xF);          /* doorbell stride */
    if ((uint16_t)(cap & 0xFFFF) < AQ_DEPTH - 1) {        /* CAP.MQES (0-based) */
        kprintf("[nvme] max queue entries %d < %d\n", (int)(cap & 0xFFFF) + 1, AQ_DEPTH);
        return -1;
    }

    /* Reset, then wait CSTS.RDY == 0. */
    w32(g_regs, REG_CC, r32(g_regs, REG_CC) & ~1u);
    for (long i = 0; i < 100000000L && (r32(g_regs, REG_CSTS) & 1); i++) barrier();
    if (r32(g_regs, REG_CSTS) & 1) {
        kprintf("[nvme] reset timeout (csts=%x)\n", r32(g_regs, REG_CSTS));
        return -1;
    }

    /* Admin queues (one frame each: 64*64B SQ = 4KiB, 64*16B CQ = 1KiB). */
    g_admin.sq = (struct nvme_sqe *)(uintptr_t)pmm_alloc();
    g_admin.cq = (struct nvme_cqe *)(uintptr_t)pmm_alloc();
    if (!g_admin.sq || !g_admin.cq) { nvme_free_all(); return -1; }
    memset(g_admin.sq, 0, 4096); memset(g_admin.cq, 0, 4096);
    g_admin.depth = AQ_DEPTH; g_admin.sq_tail = 0; g_admin.cq_head = 0; g_admin.cq_phase = 1; g_admin.cid = 0;
    g_admin.sq_db = g_regs + 0x1000 + 0 * stride;
    g_admin.cq_db = g_regs + 0x1000 + 1 * stride;
    w32(g_regs, REG_AQA, ((AQ_DEPTH - 1) << 16) | (AQ_DEPTH - 1));
    w64(g_regs, REG_ASQ, (uint64_t)(uintptr_t)g_admin.sq);
    w64(g_regs, REG_ACQ, (uint64_t)(uintptr_t)g_admin.cq);

    /* Enable: CSS=0 (NVM), MPS=0 (4KiB), IOSQES=6 (64B), IOCQES=4 (16B), EN=1. */
    w32(g_regs, REG_CC, (6u << 16) | (4u << 20) | 1u);
    for (long i = 0; i < 100000000L && !(r32(g_regs, REG_CSTS) & 1); i++) barrier();
    if (!(r32(g_regs, REG_CSTS) & 1) || (r32(g_regs, REG_CSTS) & 2)) {
        kprintf("[nvme] controller enable failed (csts=%x)\n", r32(g_regs, REG_CSTS));
        nvme_free_all();
        return -1;
    }

    uint8_t *idbuf = (uint8_t *)(uintptr_t)pmm_alloc();
    if (!idbuf) { nvme_free_all(); return -1; }
    struct nvme_sqe cmd;

    /* Identify Controller (CNS=1): MDTS (max transfer in 2^MDTS host pages) + model. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = 0; cmd.prp1 = (uint64_t)(uintptr_t)idbuf; cmd.cdw10 = 1;  /* CNS=1 */
    if (nvme_run(&g_admin, &cmd) != 0) { kprintf("[nvme] identify ctrl failed\n"); pmm_free((uint64_t)(uintptr_t)idbuf); nvme_free_all(); return -1; }
    uint8_t mdts = idbuf[77];
    if (mdts > 7) mdts = 7;                             /* avoid 1u<<mdts UB / *8 overflow */
    g_max_sectors = mdts ? ((1u << mdts) * (4096u / 512u)) : 0xFFFF;   /* MPS=0 -> 4 KiB pages */
    char model[41]; for (int i = 0; i < 40; i++) model[i] = (char)idbuf[24 + i]; model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;     /* trim trailing spaces */

    /* Active Namespace List (CNS=2): use the first active NSID, not a hardcoded 1. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = 0; cmd.prp1 = (uint64_t)(uintptr_t)idbuf; cmd.cdw10 = 2;  /* CNS=2 */
    g_nsid = (nvme_run(&g_admin, &cmd) == 0 && *(uint32_t *)idbuf) ? *(uint32_t *)idbuf : 1;

    /* Identify Namespace (chosen NSID) -> capacity + LBA size. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = g_nsid; cmd.prp1 = (uint64_t)(uintptr_t)idbuf; cmd.cdw10 = 0; /* CNS=0 */
    if (nvme_run(&g_admin, &cmd) != 0) { kprintf("[nvme] identify ns failed\n"); pmm_free((uint64_t)(uintptr_t)idbuf); nvme_free_all(); return -1; }
    g_cap = *(uint64_t *)idbuf;                          /* NSZE */
    uint8_t flbas = idbuf[26] & 0xF;
    uint8_t lbads = idbuf[128 + flbas * 4 + 2];          /* LBAF[flbas].LBADS */
    g_lba = 1u << lbads;
    pmm_free((uint64_t)(uintptr_t)idbuf);                /* done with the identify buffer */
    if (g_lba != 512) { kprintf("[nvme] unsupported lba size %d (need 512)\n", (int)g_lba); nvme_free_all(); return -1; }

    /* I/O queue pairs, qid 1..n, one per core (see NVME_IOQ_MAX above).
     *
     * CAP.MQES already bounded the depth check at the top of this function, and
     * the controller's maximum queue COUNT is a Set Features (0x07) negotiation
     * -- but Create I/O Queue answers with status 0x01 ("invalid queue
     * identifier") when it will not give another, so the loop simply STOPS at
     * the first refusal and keeps what it has. Asking and believing the answer
     * beats asking twice, and the fallback -- one queue -- is the shape that
     * shipped before this, so a controller that grants none is not a
     * regression. */
    for (int i = 0; i < NVME_IOQ_MAX; i++) {
        uint16_t qid = (uint16_t)(i + 1);
        struct nvme_q *q = &g_io[i];

        q->cq = (struct nvme_cqe *)(uintptr_t)pmm_alloc();
        if (!q->cq) break;
        memset(q->cq, 0, 4096);
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0 = 0x05; cmd.prp1 = (uint64_t)(uintptr_t)q->cq;
        cmd.cdw10 = ((IO_DEPTH - 1) << 16) | qid;        /* qsize-1 | qid */
        cmd.cdw11 = 1;                                   /* PC=1, IEN=0 (polled) */
        if (nvme_run(&g_admin, &cmd) != 0) {
            pmm_free((uint64_t)(uintptr_t)q->cq); q->cq = NULL;
            break;
        }

        q->sq = (struct nvme_sqe *)(uintptr_t)pmm_alloc();
        if (!q->sq) { break; }
        memset(q->sq, 0, 4096);
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0 = 0x01; cmd.prp1 = (uint64_t)(uintptr_t)q->sq;
        cmd.cdw10 = ((IO_DEPTH - 1) << 16) | qid;        /* qsize-1 | qid */
        cmd.cdw11 = ((uint32_t)qid << 16) | 1u;          /* CQID | PC=1 */
        if (nvme_run(&g_admin, &cmd) != 0) {
            pmm_free((uint64_t)(uintptr_t)q->sq); q->sq = NULL;
            break;
        }

        g_prp_list[i] = (uint64_t *)(uintptr_t)pmm_alloc();
        if (!g_prp_list[i]) break;

        q->depth = IO_DEPTH; q->sq_tail = 0; q->cq_head = 0; q->cq_phase = 1; q->cid = 0;
        q->sq_db = g_regs + 0x1000 + (2 * qid + 0) * stride;
        q->cq_db = g_regs + 0x1000 + (2 * qid + 1) * stride;
        g_nioq = i + 1;
    }
    if (!g_io[0].sq || !g_io[0].cq || !g_prp_list[0]) {
        kprintf("[nvme] no usable I/O queue\n");
        nvme_free_all();
        return -1;
    }

    g_ready = 1;
    kprintf("[nvme] up (slot %d %x:%x '%s') ns=%d lba=%d cap=%u sectors maxxfer=%d ioq=%d\n",
            dev.slot, dev.vendor, dev.device, model, (int)g_nsid, (int)g_lba,
            (unsigned)g_cap, (int)g_max_sectors, g_nioq);
    nvme_health_report();
    return 0;
}

/* ---------------------------------------------------------------------------
 * THE BLOCK PATH: one implementation, driven across polls
 *
 * This used to be nvme_io() -- a while loop over MDTS-sized chunks, each one
 * submitted and spun on inside the loop -- plus five public entry points that
 * all called it. There is now ONE state machine, and the loop's cursor lives in
 * the caller's `struct blk_req` instead of on this function's stack, which is
 * the whole of what "asynchronous" means here: the transfer survives the return.
 *
 * The five entry points are GONE rather than kept as wrappers. A wrapper would
 * be a second way into the device, and blkdev.h argues at length why two ways
 * is how a synchronous path and an asynchronous one come to disagree about
 * ordering. Nothing outside this file called them (grepped across c/, tests/
 * and tools/ before deleting).
 * ------------------------------------------------------------------------- */

/* Which queue this core submits on. `index` is 0 for the BSP and is valid from
 * percpu_bsp_init(), long before blk_init() -- but clamp anyway: a core beyond
 * the last queue created must still be able to do I/O, just not privately. */
static int nvme_qidx(void)
{
    int i = this_cpu()->index;
    if (i < 0 || i >= g_nioq) i = 0;
    return i;
}

/* Build and issue the command for the chunk at r->done. `tag` carries the queue
 * index AND the command id, because the poll that reports this chunk complete
 * may run on a different core from the submit -- so "which queue" cannot be
 * re-derived from the caller, it has to be remembered. */
static int nvme_issue(struct blk_req *r)
{
    int qi = (int)(r->tag >> 16);
    struct nvme_q *q = &g_io[qi];
    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof cmd);

    if (r->op == BLK_OP_FLUSH) {
        /* NVM opcode 0x00: Flush. Completing it means the namespace's volatile
         * write cache is on non-volatile media. Without it a controller with a
         * write cache is free to have the journal's commit record on media
         * while the blocks it vouches for are not -- the one state the journal
         * exists to make impossible. */
        cmd.cdw0 = 0x00;
        cmd.nsid = g_nsid;
        r->chunk = 0;
    } else {
        uint32_t n = r->count - r->done;
        if (n > g_max_sectors) n = g_max_sectors;
        if (n > 2048) n = 2048;                         /* keep the PRP list in one page */

        uint64_t addr  = (uint64_t)(uintptr_t)r->buf + (uint64_t)r->done * 512;
        uint64_t bytes = (uint64_t)n * 512;
        uint64_t first = addr & ~0xFFFULL;
        uint64_t last  = (addr + bytes - 1) & ~0xFFFULL;
        int npages = (int)((last - first) / 0x1000) + 1;

        cmd.cdw0 = (r->op == BLK_OP_WRITE) ? 0x01 : 0x02;   /* Write : Read */
        cmd.nsid = g_nsid;
        cmd.prp1 = addr;
        if (npages == 1)      cmd.prp2 = 0;
        else if (npages == 2) cmd.prp2 = first + 0x1000;
        else {
            for (int i = 1; i < npages; i++) g_prp_list[qi][i - 1] = first + (uint64_t)i * 0x1000;
            cmd.prp2 = (uint64_t)(uintptr_t)g_prp_list[qi];
        }
        cmd.cdw10 = (uint32_t)((r->dev_lba + r->done) & 0xFFFFFFFFu);
        cmd.cdw11 = (uint32_t)((r->dev_lba + r->done) >> 32);
        cmd.cdw12 = n - 1;                                  /* NLB, 0-based */
        r->chunk = n;
    }

    uint16_t cid = nvme_begin(q, &cmd);
    r->tag = ((uint32_t)qi << 16) | cid;
    /* A millisecond deadline, not a spin count: this poll may be called once
     * every scheduler tick by an async waiter, so "how many times have I
     * looked" says nothing about how long the controller has had. 10 s is the
     * NVMe default command timeout and far outside anything QEMU does. */
    r->deadline = timer_ms() + 10000;
    return 0;
}

int nvme_blk_submit(struct blk_req *r)
{
    if (!g_ready) return -1;
    if (r->op != BLK_OP_FLUSH && r->count == 0) return -1;
    r->tag = (uint32_t)nvme_qidx() << 16;
    return nvme_issue(r);
}

int nvme_blk_poll(struct blk_req *r)
{
    struct nvme_q *q = &g_io[r->tag >> 16];
    int status = 0;

    if (!nvme_step(q, (uint16_t)(r->tag & 0xFFFF), &status)) {
        if (timer_ms() > r->deadline) {
            /* NO RETRY, deliberately, and unlike AHCI. An AHCI timeout is a
             * port that latched an error and can be recovered; an NVMe command
             * that has not completed in ten seconds is still OWNED BY THE
             * CONTROLLER -- its PRPs point at this buffer and it may write them
             * at any moment. Re-issuing would put two commands on one buffer.
             * Abort/reset is the only correct recovery and this driver has
             * neither, so fail out loud rather than invent one. */
            kprintf("[nvme] command timeout (q%d cid %d, lba %u)\n",
                    (int)(r->tag >> 16), (int)(r->tag & 0xFFFF),
                    (unsigned)(r->dev_lba + r->done));
            r->status = -1;
            return 1;
        }
        return 0;
    }
    if (status != 0) { r->status = -1; return 1; }

    r->done += r->chunk;
    if (r->op != BLK_OP_FLUSH && r->done < r->count) {
        if (nvme_issue(r) != 0) { r->status = -1; return 1; }
        return 0;                                       /* the next chunk is in flight */
    }
    r->status = 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * SMART / health log page (Get Log Page, LID 0x02)
 *
 * The observability gap this closes, stated plainly: before this there was no
 * way, anywhere in this tree, to ask a disk how it is doing. A drive reporting
 * that it is out of spare blocks, or running at 85 C, or that it has logged
 * media errors, reached not one line of any boot log.
 *
 * Read once at bring-up and PRINTED, so the answer lands in every serial
 * capture every boot harness already keeps -- which is what makes it evidence
 * rather than a function somebody could call. nvme_health() re-reads on demand
 * for a caller that wants a fresh count.
 *
 * The 128-bit counters are truncated to their low 64 bits ON PURPOSE, recorded
 * here rather than hidden in a cast: 2^64 data units is 9.4 zettabytes, and
 * this kernel's kprintf cannot print a 128-bit number anyway.
 * ------------------------------------------------------------------------- */
static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

int nvme_health(struct nvme_health *out)
{
    if (!g_ready || !out) return -1;

    /* The controller DMAs the log page, so it needs an identity-mapped,
     * page-aligned landing area -- a stack buffer is neither. One frame, taken
     * and returned; this is off the page-fault path, so allocating is allowed
     * here in a way it is not in swap.c. */
    uint8_t *buf = (uint8_t *)(uintptr_t)pmm_alloc();
    if (!buf) return -1;
    memset(buf, 0, 4096);

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = 0x02;                                /* Get Log Page */
    cmd.nsid  = 0xFFFFFFFFu;                         /* controller-wide, not per-namespace */
    cmd.prp1  = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = 0x02u | (((512u / 4u) - 1u) << 16);  /* LID 0x02 | NUMD (0-based dwords) */
    if (nvme_run(&g_admin, &cmd) != 0) { pmm_free((uint64_t)(uintptr_t)buf); return -1; }

    out->critical_warning = buf[0];
    out->temp_kelvin      = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
    out->spare_pct        = buf[3];
    out->spare_threshold  = buf[4];
    out->used_pct         = buf[5];
    out->data_read        = le64(buf + 32);
    out->data_written     = le64(buf + 48);
    out->power_cycles     = le64(buf + 112);
    out->power_on_hours   = le64(buf + 128);
    out->unsafe_shutdowns = le64(buf + 144);
    out->media_errors     = le64(buf + 160);
    out->error_entries    = le64(buf + 176);

    pmm_free((uint64_t)(uintptr_t)buf);
    g_health = *out;
    g_health_ok = 1;
    return 0;
}

void nvme_health_report(void)
{
    struct nvme_health h;
    if (nvme_health(&h) != 0) {
        /* Not fatal, and not silent. A controller that refuses the log page has
         * an UNKNOWN health, which is a different thing from a healthy one, and
         * a missing line would read as the second. */
        kprintf("[nvme] SMART/health log unavailable -- health is UNKNOWN\n");
        return;
    }
    /* Kelvin is what the spec reports; 273 is subtracted here and not in the
     * accessor, so a caller still gets the raw field. */
    kprintf("[nvme] health: warn=%x temp=%dC spare=%d%%/%d%% used=%d%% "
            "read=%u written=%u units\n",
            (unsigned)h.critical_warning,
            (int)h.temp_kelvin - 273, (int)h.spare_pct, (int)h.spare_threshold,
            (int)h.used_pct, (unsigned)h.data_read, (unsigned)h.data_written);
    kprintf("[nvme] health: power_cycles=%u hours=%u unsafe_shutdowns=%u "
            "media_errors=%u log_entries=%u\n",
            (unsigned)h.power_cycles, (unsigned)h.power_on_hours,
            (unsigned)h.unsafe_shutdowns, (unsigned)h.media_errors,
            (unsigned)h.error_entries);
    if (h.critical_warning)
        kprintf("[nvme] CRITICAL WARNING %x -- the drive is reporting a fault\n",
                (unsigned)h.critical_warning);
}

int nvme_health_known(void) { return g_health_ok; }
