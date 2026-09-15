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
/* DMA migration correction: queue/admin/PRP memory now has separate CPU and
 * bus addresses, and caller data uses per-command SG mappings. The historical
 * identity/bounce-only description above no longer specifies the data path. */
#include <stdint.h>
#include <stddef.h>
#include "nvme.h"
#include "blkdev.h"
#include "pci.h"
#include "vmm.h"
#include "dma.h"
#include "driver.h"
#include "panic.h"
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

/* DMA correction: CPU aliases and bus addresses are independent. Queues stay
 * device-owned while enabled; streaming payload mappings survive async polls. */
struct nvme_q {
    struct dma_buffer *sq_mem, *cq_mem;
    struct dma_mapping *mapping;
    uint64_t map_token, prp_token;

    struct nvme_sqe *sq;          /* page-aligned DMA */
    struct nvme_cqe *cq;
    volatile uint8_t *sq_db, *cq_db;
    uint16_t sq_tail, cq_head, depth, cid;
    uint8_t  cq_phase;
    uint32_t result;             /* DWORD0 of the last completed command */
};

static struct dma_device g_dma;
static struct dma_buffer *g_prp_mem[NVME_IOQ_MAX];
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
static struct device *g_device;
static void nvme_remove(struct device *dev);
static const struct driver nvme_driver = {
    .name = "nvme", .bus_type = DEV_BUS_PCI, .remove = nvme_remove,
};

volatile int g_nvme_busy = 0;          /* interrupts.c: don't preempt mid-poll */
int nvme_busy(void)    { return g_nvme_busy; }
int nvme_present(void) { return g_ready; }
uint64_t nvme_capacity(void) { return g_ready ? g_cap : 0; }

/* The old driver rescanned bus 0/function 0 and mapped a guessed 32 KiB BAR.
 * That worked for a QEMU endpoint at 00:04.0 but missed NVMe behind real PCIe
 * root ports, and could access outside a small BAR. Use the already-enumerated
 * function and its sized resource; discovery belongs to the PCI core.
 * Measured 2026-09-10: test-nvme-bridge placed the only root disk at 01:00.0
 * behind a QEMU PCIe root port; eight filesystem mutation/fsync assertions and
 * the final 127-byte image survived a second cold boot. No physical PC was
 * exercised by that test; controller timing still needs hardware validation. */
static struct device *nvme_find(void)
{
    struct device *dev = NULL;
    while ((dev = dev_find_class(0x01, 0x08, dev))) {
        if (dev->bus_type == DEV_BUS_PCI && dev->prog_if == 0x02 && !dev->drv)
            return dev;
    }
    return NULL;
}

/* This implementation chooses 4 KiB pages and the NVM command set. Return the
 * number of I/O queues whose doorbells actually fit in the BAR. CAP.DSTRD can
 * make the last doorbell much farther away than 0x1000 + queue_count * 8. */
static unsigned nvme_controller_ioqs(uint64_t cap, uint64_t bar_bytes)
{
    if (((cap >> 48) & 15) != 0 || !(cap & (UINT64_C(1) << 37)) ||
        (cap & 0xffff) < AQ_DEPTH - 1) return 0;
    uint64_t stride = UINT64_C(4) << ((cap >> 32) & 15);
    unsigned count = 0;
    for (unsigned qid = 1; qid <= NVME_IOQ_MAX; ++qid) {
        if (UINT64_C(0x1000) + (2 * qid + 1) * stride + 4 > bar_bytes) break;
        ++count;
    }
    return count;
}

/* Set Features / Number of Queues returns separate zero-based SQ and CQ
 * allocations. The BAR bound is not a grant from the controller: creating four
 * pairs after it granted one is an invalid initialization sequence. */
static unsigned nvme_granted_ioqs(uint32_t result, unsigned requested)
{
    unsigned sq = (result & 0xffffu) + 1;
    unsigned cq = (result >> 16) + 1;
    unsigned granted = sq < cq ? sq : cq;
    return granted < requested ? granted : requested;
}

/* Identify Namespace uses a byte-sized LBADS exponent, not a trusted C shift
 * count. Extended FLBAS bits 6:5 select formats 16..63 on newer controllers.
 * A metadata/protection format needs a different PRP/data contract, so reject
 * it before publishing a disk instead of submitting every I/O with MPTR=0. */
static int nvme_namespace_format(const uint8_t *id, uint64_t *sectors, uint32_t *lba)
{
    unsigned format = (id[26] & 15u) | ((id[26] & 0x60u) >> 1);
    if ((id[26] & 0x80u) || format > id[25] || format >= 64) return -1;
    const uint8_t *entry = id + 128 + format * 4;
    if (entry[0] || entry[1] || entry[2] != 9 || (id[29] & 7)) return -1;
    uint64_t capacity = 0;
    for (unsigned i = 0; i < 8; ++i) capacity |= (uint64_t)id[i] << (8 * i);
    if (!capacity) return -1;
    *sectors = capacity; *lba = 512;
    return 0;
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

    dma_rmb();
    uint16_t ecid  = e->cid;
    uint16_t estat = (uint16_t)((e->status >> 1) & 0x7FFF);
    uint32_t result = e->result;
    q->cq_head = (uint16_t)((q->cq_head + 1) % q->depth);
    if (q->cq_head == 0) q->cq_phase ^= 1;
    *(volatile uint32_t *)q->cq_db = q->cq_head;
    if (ecid != cid) return 0;
    q->result = result;
    *status = estat;
    return 1;
}

/* Begin + step to completion, for the bring-up path only.
 *
 * Kept as a spin COUNT rather than a millisecond deadline on purpose: this
 * runs from kmain before the scheduler exists and, at the point of the very
 * first admin command, with the PIT not yet advancing -- a ms deadline there
 * never fires. Returns the status code (0 = success, <0 = timeout). */
static void nvme_quiesce(void);

static int nvme_run(struct nvme_q *q, const struct nvme_sqe *cmd)
{
    uint64_t fl; __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    __atomic_fetch_add(&g_nvme_busy, 1, __ATOMIC_RELAXED);
    __asm__ volatile ("sti");
    uint16_t cid = nvme_begin(q, cmd);

    int status = -1;
    for (long spins = 0; spins < 200000000L; spins++)
        if (nvme_step(q, cid, &status)) break;

    if (!(fl & 0x200)) __asm__ volatile ("cli");
    __atomic_fetch_sub(&g_nvme_busy, 1, __ATOMIC_RELAXED);
    if (status < 0) nvme_quiesce();
    return status;
}

/* Disable and OBSERVE RDY clear before invalidating DMA ownership. A failed
 * stop cannot return: pinning does not stop a caller overwriting its buffer. */
static void nvme_quiesce(void)
{
    g_ready = 0;
    w32(g_regs, REG_CC, 0);
    for (long i = 0; i < 100000000L && (r32(g_regs, REG_CSTS) & 1); i++) barrier();
    if (r32(g_regs, REG_CSTS) & 1) {
        dma_device_quarantine(&g_dma);
        panic("nvme: DMA stop unconfirmed; caller memory cannot be reused");
    }
    dma_device_quiesced(&g_dma);
}

static void nvme_free_all(void)
{
    nvme_quiesce();
    if (g_admin.sq_mem) dma_free_coherent(g_admin.sq_mem);
    if (g_admin.cq_mem) dma_free_coherent(g_admin.cq_mem);
    memset(&g_admin, 0, sizeof g_admin);
    for (int i = 0; i < NVME_IOQ_MAX; i++) {
        if (g_io[i].mapping) dma_unmap(g_io[i].mapping);
        if (g_io[i].sq_mem) dma_free_coherent(g_io[i].sq_mem);
        if (g_io[i].cq_mem) dma_free_coherent(g_io[i].cq_mem);
        if (g_prp_mem[i]) dma_free_coherent(g_prp_mem[i]);
        memset(&g_io[i], 0, sizeof g_io[i]);
        g_prp_mem[i] = NULL; g_prp_list[i] = NULL;
    }
    g_nioq = 1;
}

/* Init failures after CSTS.RDY reached zero share one containment path.  The
 * controller stop acknowledgement makes its submitted buffers reclaimable;
 * PCI Command readback is a separate final gate. */
static int nvme_init_release(struct device *dev)
{
    nvme_free_all();
    if (dev_disable_checked(dev) != 0)
        kprintf("[nvme] PCI Command disable unconfirmed after failed init\n");
    g_regs = NULL;
    g_health_ok = 0;
    return -1;
}

static int nvme_queue_alloc(struct nvme_q *q)
{
    q->sq_mem = dma_alloc_coherent(&g_dma, 4096, 4096, 0);
    q->cq_mem = dma_alloc_coherent(&g_dma, 4096, 4096, 0);
    q->sq = q->sq_mem ? q->sq_mem->cpu : NULL;
    q->cq = q->cq_mem ? q->cq_mem->cpu : NULL;
    return q->sq && q->cq ? 0 : -1;
}

/* Temporary admin payloads have exactly one command's DMA lifetime. */
static int nvme_admin_data(struct nvme_sqe *cmd, struct dma_buffer *buffer)
{
    cmd->prp1 = dma_addr_value(buffer->dma);
    uint64_t token = dma_buffer_submit(buffer);
    if (!token) return -1;
    int status = nvme_run(&g_admin, cmd);
    /* nvme_run already confirmed controller quiescence on timeout. */
    if (status >= 0 && dma_buffer_complete(buffer, token)) return -1;
    return status;
}

int nvme_init(void)
{
    struct device *dev = nvme_find();
    if (!dev) return -1;
    if (!(dev->res[0].flags & DEV_RES_MEM) || !dev->res[0].start ||
        dev->res[0].size < 0x1000) {
        kprintf("[nvme] BAR0 absent or too small\n"); return -1;
    }
    /* Decode registers while explicitly containing firmware DMA.  This
     * readback is only the PCI gate; CC/CSTS below establish controller stop. */
    if (dev_enable_checked(dev, 0) != 0) {
        kprintf("[nvme] cannot enable BAR decode with bus mastering off\n");
        return -1;
    }
    uint64_t bar0 = dev_bar_map(dev, 0);
    if (!bar0) {
        kprintf("[nvme] invalid BAR0\n");
        (void)dev_disable_checked(dev);
        return -1;
    }
    g_regs = (volatile uint8_t *)bar0;
    dma_device_init(&g_dma, "nvme", DMA_MASK_64);
    g_dma.max_segments = 257;

    uint64_t cap = r64(g_regs, REG_CAP);
    uint32_t stride = 4u << ((cap >> 32) & 0xF);          /* doorbell stride */
    unsigned wanted_ioqs = nvme_controller_ioqs(cap, dev->res[0].size);
    if (!wanted_ioqs) {
        kprintf("[nvme] unsupported controller: need NVM, 4KiB pages, depth 64 and mapped doorbells\n");
        (void)dev_disable_checked(dev);
        g_regs = NULL;
        return -1;
    }

    /* Reset, then wait CSTS.RDY == 0. */
    w32(g_regs, REG_CC, r32(g_regs, REG_CC) & ~1u);
    for (long i = 0; i < 100000000L && (r32(g_regs, REG_CSTS) & 1); i++) barrier();
    if (r32(g_regs, REG_CSTS) & 1) {
        kprintf("[nvme] reset timeout (csts=%x)\n", r32(g_regs, REG_CSTS));
        /* BME was already confirmed clear by the MEM-only gate. */
        (void)dev_disable_checked(dev);
        g_regs = NULL;
        return -1;
    }

    /* Admin queues (one frame each: 64*64B SQ = 4KiB, 64*16B CQ = 1KiB). */
    if (nvme_queue_alloc(&g_admin)) return nvme_init_release(dev);
    memset(g_admin.sq, 0, 4096); memset(g_admin.cq, 0, 4096);
    g_admin.depth = AQ_DEPTH; g_admin.sq_tail = 0; g_admin.cq_head = 0; g_admin.cq_phase = 1; g_admin.cid = 0;
    g_admin.sq_db = g_regs + 0x1000 + 0 * stride;
    g_admin.cq_db = g_regs + 0x1000 + 1 * stride;
    w32(g_regs, REG_AQA, ((AQ_DEPTH - 1) << 16) | (AQ_DEPTH - 1));
    w64(g_regs, REG_ASQ, dma_addr_value(g_admin.sq_mem->dma));
    w64(g_regs, REG_ACQ, dma_addr_value(g_admin.cq_mem->dma));
    if (!dma_buffer_submit(g_admin.sq_mem) || !dma_buffer_submit(g_admin.cq_mem)) {
        return nvme_init_release(dev);
    }

    /* The controller is still RDY=0 and the admin queue pair is now the only
     * published DMA state.  Grant bus mastering only at this boundary. */
    if (dev_enable_checked(dev, 1) != 0) {
        kprintf("[nvme] PCI bus-master enable rejected\n");
        return nvme_init_release(dev);
    }

    /* Enable: CSS=0 (NVM), MPS=0 (4KiB), IOSQES=6 (64B), IOCQES=4 (16B), EN=1. */
    w32(g_regs, REG_CC, (6u << 16) | (4u << 20) | 1u);
    for (long i = 0; i < 100000000L && !(r32(g_regs, REG_CSTS) & 1); i++) barrier();
    if (!(r32(g_regs, REG_CSTS) & 1) || (r32(g_regs, REG_CSTS) & 2)) {
        kprintf("[nvme] controller enable failed (csts=%x)\n", r32(g_regs, REG_CSTS));
        return nvme_init_release(dev);
    }

    struct dma_buffer *idmem = dma_alloc_coherent(&g_dma, 4096, 4096, 0);
    uint8_t *idbuf = idmem ? idmem->cpu : NULL;
    if (!idbuf) return nvme_init_release(dev);
    struct nvme_sqe cmd;

    /* Identify Controller (CNS=1): MDTS (max transfer in 2^MDTS host pages) + model. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = 0; cmd.prp1 = dma_addr_value(idmem->dma); cmd.cdw10 = 1;  /* CNS=1 */
    if (nvme_admin_data(&cmd, idmem) != 0) {
        kprintf("[nvme] identify ctrl failed\n");
        dma_free_coherent(idmem); return nvme_init_release(dev);
    }
    uint8_t mdts = idbuf[77];
    if (mdts > 7) mdts = 7;                             /* avoid 1u<<mdts UB / *8 overflow */
    g_max_sectors = mdts ? ((1u << mdts) * (4096u / 512u)) : 0xFFFF;   /* MPS=0 -> 4 KiB pages */
    char model[41]; for (int i = 0; i < 40; i++) model[i] = (char)idbuf[24 + i]; model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;     /* trim trailing spaces */

    /* Active Namespace List (CNS=2): use the first active NSID, not a hardcoded 1. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = 0; cmd.prp1 = dma_addr_value(idmem->dma); cmd.cdw10 = 2;  /* CNS=2 */
    int nsstatus = nvme_admin_data(&cmd, idmem);
    if (nsstatus < 0) { dma_free_coherent(idmem); return nvme_init_release(dev); }
    g_nsid = (nsstatus == 0 && *(uint32_t *)idbuf) ? *(uint32_t *)idbuf : 1;

    /* Identify Namespace (chosen NSID) -> capacity + LBA size. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = g_nsid; cmd.prp1 = dma_addr_value(idmem->dma); cmd.cdw10 = 0; /* CNS=0 */
    if (nvme_admin_data(&cmd, idmem) != 0) {
        kprintf("[nvme] identify ns failed\n");
        dma_free_coherent(idmem); return nvme_init_release(dev);
    }
    int format_ok = nvme_namespace_format(idbuf, &g_cap, &g_lba);
    dma_free_coherent(idmem);
    if (format_ok) {
        kprintf("[nvme] unsupported namespace: need nonempty 512-byte LBAs without metadata/protection\n");
        return nvme_init_release(dev);
    }

    /* I/O queue pairs, qid 1..n, one per core (see NVME_IOQ_MAX above).
     * Historical behavior: Create Queue rejection was treated as queue-count
     * negotiation. That could work in QEMU, but a physical controller may grant
     * fewer queues, and the NVMe initialization sequence requires requesting
     * Number of Queues before creating them. Read the actual SQ/CQ allocations
     * from completion DWORD0, independently of the BAR-size/local bounds. */
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x09; cmd.cdw10 = 0x07; /* Set Features: Number of Queues */
    cmd.cdw11 = ((wanted_ioqs - 1) << 16) | (wanted_ioqs - 1);
    if (nvme_run(&g_admin, &cmd) != 0) {
        kprintf("[nvme] queue-count negotiation failed\n");
        return nvme_init_release(dev);
    }
    wanted_ioqs = nvme_granted_ioqs(g_admin.result, wanted_ioqs);
    kprintf("[nvme] queue grant=%u pair(s)\n", wanted_ioqs);
    for (unsigned i = 0; i < wanted_ioqs; i++) {
        uint16_t qid = (uint16_t)(i + 1);
        struct nvme_q *q = &g_io[i];

        /* Allocate the complete pair before publishing either address. Partial
         * allocation is still CPU-owned and may be unwound immediately. */
        if (nvme_queue_alloc(q)) return nvme_init_release(dev);
        g_prp_mem[i] = dma_alloc_coherent(&g_dma, 4096, 4096, 0);
        if (!g_prp_mem[i]) return nvme_init_release(dev);
        g_prp_list[i] = g_prp_mem[i]->cpu;
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0 = 0x05; cmd.prp1 = dma_addr_value(q->cq_mem->dma);
        cmd.cdw10 = ((IO_DEPTH - 1) << 16) | qid;
        cmd.cdw11 = 1;
        uint64_t cqtoken = dma_buffer_submit(q->cq_mem);
        int cs = cqtoken ? nvme_run(&g_admin, &cmd) : -1;
        if (cs != 0) {
            if (cs < 0 || i == 0) return nvme_init_release(dev);
            dma_buffer_complete(q->cq_mem, cqtoken); /* rejected: controller never adopted it */
            dma_free_coherent(q->cq_mem); dma_free_coherent(q->sq_mem);
            dma_free_coherent(g_prp_mem[i]); g_prp_mem[i] = NULL; g_prp_list[i] = NULL;
            memset(q, 0, sizeof *q); break;
        }
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0 = 0x01; cmd.prp1 = dma_addr_value(q->sq_mem->dma);
        cmd.cdw10 = ((IO_DEPTH - 1) << 16) | qid;
        cmd.cdw11 = ((uint32_t)qid << 16) | 1u;
        if (!dma_buffer_submit(q->sq_mem) || nvme_run(&g_admin, &cmd) != 0) {
            /* CQ is already live: stop the controller before any unwind. */
            return nvme_init_release(dev);
        }

        q->depth = IO_DEPTH; q->sq_tail = 0; q->cq_head = 0; q->cq_phase = 1; q->cid = 0;
        q->sq_db = g_regs + 0x1000 + (2 * qid + 0) * stride;
        q->cq_db = g_regs + 0x1000 + (2 * qid + 1) * stride;
        g_nioq = i + 1;
    }
    if (!g_io[0].sq || !g_io[0].cq || !g_prp_list[0]) {
        kprintf("[nvme] no usable I/O queue\n");
        return nvme_init_release(dev);
    }

    /* Storage comes up before the model's late probe pass. Bind the already
     * enumerated function now so dev_unbind reaches the normal DMA teardown. */
    g_device = dev;
    dev->drv = &nvme_driver; dev->drvdata = &g_dma;
    g_ready = 1;
    kprintf("[nvme] up (slot %d %x:%x '%s') ns=%d lba=%d cap=%u sectors maxxfer=%d ioq=%d\n",
            dev->slot, dev->vendor, dev->device, model, (int)g_nsid, (int)g_lba,
            (unsigned)g_cap, (int)g_max_sectors, g_nioq);
    kprintf("[nvme] PCI %02x:%02x.%u BAR0=%llx size=%llu\n",
            dev->bus, dev->slot, dev->func, (unsigned long long)bar0,
            (unsigned long long)dev->res[0].size);
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

        void *cpu = (uint8_t *)r->buf + (uint64_t)r->done * 512;
        size_t bytes = (size_t)n * 512;
        q->mapping = dma_map_kernel(&g_dma, cpu, bytes,
            r->op == BLK_OP_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE, 0);
        if (!q->mapping) return -1;
        uint64_t addr = dma_addr_value(dma_mapping_addr(q->mapping, 0));
        size_t offset = 4096 - (addr & 4095);
        cmd.cdw0 = r->op == BLK_OP_WRITE ? 0x01 : 0x02;
        cmd.nsid = g_nsid; cmd.prp1 = addr;
        if (bytes > offset) {
            if (bytes - offset <= 4096)
                cmd.prp2 = dma_addr_value(dma_mapping_addr(q->mapping, offset));
            else {
                unsigned ent = 0;
                for (; offset < bytes; offset += 4096)
                    g_prp_list[qi][ent++] = dma_addr_value(dma_mapping_addr(q->mapping, offset));
                cmd.prp2 = dma_addr_value(g_prp_mem[qi]->dma);
                q->prp_token = dma_buffer_submit(g_prp_mem[qi]);
                if (!q->prp_token) { dma_unmap(q->mapping); q->mapping = NULL; return -1; }
            }
        }
        q->map_token = dma_mapping_submit(q->mapping);
        if (!q->map_token) {
            if (q->prp_token) dma_buffer_complete(g_prp_mem[qi], q->prp_token);
            q->prp_token = 0; dma_unmap(q->mapping); q->mapping = NULL; return -1;
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

    /* An admin timeout stops the entire controller, including a block request
     * parked between polls. Its old CQ entry and tokens are no longer valid.
     * nvme_quiesce only returns after RDY cleared, so release this request's
     * mapping without interpreting or acknowledging a pre-reset completion. */
    if (!g_ready) {
        if (q->mapping) {
            if (dma_unmap(q->mapping)) panic("nvme: stopped DMA still owned");
            q->mapping = NULL;
        }
        q->map_token = q->prp_token = 0;
        r->status = -1;
        return 1;
    }

    if (!nvme_step(q, (uint16_t)(r->tag & 0xFFFF), &status)) {
        if (timer_ms() > r->deadline) {
            /* NO RETRY, deliberately, and unlike AHCI. An AHCI timeout is a
             * port that latched an error and can be recovered; an NVMe command
             * that has not completed in ten seconds is still OWNED BY THE
             * CONTROLLER -- its PRPs point at this buffer and it may write them
             * at any moment. Re-issuing would put two commands on one buffer.
             * Abort/reset is the only correct recovery and this driver has
             * neither, so fail out loud rather than invent one. */
            /* DMA migration: the historical no-reset limitation above is
             * superseded by acknowledged CC.EN/CSTS.RDY stop below. */
            kprintf("[nvme] command timeout (q%d cid %d, lba %u)\n",
                    (int)(r->tag >> 16), (int)(r->tag & 0xFFFF),
                    (unsigned)(r->dev_lba + r->done));
            nvme_quiesce();
            if (q->mapping) { dma_unmap(q->mapping); q->mapping = NULL; }
            q->map_token = q->prp_token = 0;
            r->status = -1;
            return 1;
        }
        return 0;
    }
    if (q->mapping) {
        int dc = dma_mapping_complete(q->mapping, q->map_token,
                                      status == 0 ? (size_t)r->chunk * 512 : 0);
        if (dc || dma_unmap(q->mapping)) panic("nvme: invalid DMA completion ownership");
        q->mapping = NULL; q->map_token = 0;
    }
    if (q->prp_token) {
        if (dma_buffer_complete(g_prp_mem[r->tag >> 16], q->prp_token))
            panic("nvme: invalid PRP completion ownership");
        q->prp_token = 0;
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
    struct dma_buffer *memory = dma_alloc_coherent(&g_dma, 4096, 4096, 0);
    uint8_t *buf = memory ? memory->cpu : NULL;
    if (!buf) return -1;
    memset(buf, 0, 4096);

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = 0x02;                                /* Get Log Page */
    cmd.nsid  = 0xFFFFFFFFu;                         /* controller-wide, not per-namespace */
    cmd.prp1  = dma_addr_value(memory->dma);
    cmd.cdw10 = 0x02u | (((512u / 4u) - 1u) << 16);  /* LID 0x02 | NUMD (0-based dwords) */
    if (nvme_admin_data(&cmd, memory) != 0) { dma_free_coherent(memory); return -1; }

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

    dma_free_coherent(memory);
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

/* BKL-held normal teardown: prevent upper-layer submissions and finish the
 * current request before invalidating the queues it polls. Stop acknowledgement
 * is mandatory even after a prior timeout; failed hardware stop never returns. */
void nvme_shutdown(void)
{
    if (!g_regs) return;
    struct blkdev *disk = blk_find("nvme0");
    if (disk) blk_dev_offline(disk);
    nvme_free_all();
    if (g_device && dev_disable_checked(g_device) != 0)
        kprintf("[nvme] PCI Command disable unconfirmed after controller stop\n");
    g_regs = NULL; g_health_ok = 0;
}

static void nvme_remove(struct device *dev)
{
    (void)dev;
    nvme_shutdown();
    g_device = NULL;
}
