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
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
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
static struct nvme_q g_admin, g_io;
static uint32_t g_nsid = 1, g_lba = 512;
static uint64_t g_cap;                 /* capacity in LBAs */
static uint64_t *g_prp_list;           /* one page: up to 512 PRP entries for large DMA */
static uint32_t g_max_sectors = 0xFFFF;/* per-command cap from the controller's MDTS */
static int g_ready = 0;

volatile int g_nvme_busy = 0;          /* interrupts.c: don't preempt mid-poll */
int nvme_busy(void)    { return g_nvme_busy; }
int nvme_present(void) { return g_ready; }

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

/* Submit one command on queue q, ring the SQ tail doorbell, poll the CQ for the
 * phase-bit flip, ring the CQ head doorbell. Returns the status code (0 = success,
 * <0 = timeout). Mirrors virtio.c's IF-on/non-preemptible poll discipline. */
static int nvme_submit(struct nvme_q *q, const struct nvme_sqe *cmd)
{
    uint16_t cid = q->cid++;
    struct nvme_sqe *slot = &q->sq[q->sq_tail];
    memset(slot, 0, sizeof *slot);
    *slot = *cmd;
    slot->cdw0 = (slot->cdw0 & 0xFFFF) | ((uint32_t)cid << 16);
    barrier();
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);

    uint64_t fl; __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    g_nvme_busy++;
    __asm__ volatile ("sti");
    *(volatile uint32_t *)q->sq_db = q->sq_tail;

    int status = -1;
    for (long spins = 0; spins < 200000000L; spins++) {
        barrier();
        volatile struct nvme_cqe *e = &q->cq[q->cq_head];
        if ((e->status & 1) == q->cq_phase) {
            uint16_t ecid = e->cid;
            uint16_t estat = (uint16_t)((e->status >> 1) & 0x7FFF);
            q->cq_head = (uint16_t)((q->cq_head + 1) % q->depth);
            if (q->cq_head == 0) q->cq_phase ^= 1;
            *(volatile uint32_t *)q->cq_db = q->cq_head;
            if (ecid != cid) continue;      /* stale CQE from a timed-out command */
            status = estat;
            break;
        }
    }
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
    if (g_io.sq)    { pmm_free((uint64_t)(uintptr_t)g_io.sq);    g_io.sq = NULL; }
    if (g_io.cq)    { pmm_free((uint64_t)(uintptr_t)g_io.cq);    g_io.cq = NULL; }
    if (g_prp_list) { pmm_free((uint64_t)(uintptr_t)g_prp_list); g_prp_list = NULL; }
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
    if (nvme_submit(&g_admin, &cmd) != 0) { kprintf("[nvme] identify ctrl failed\n"); pmm_free((uint64_t)(uintptr_t)idbuf); nvme_free_all(); return -1; }
    uint8_t mdts = idbuf[77];
    if (mdts > 7) mdts = 7;                             /* avoid 1u<<mdts UB / *8 overflow */
    g_max_sectors = mdts ? ((1u << mdts) * (4096u / 512u)) : 0xFFFF;   /* MPS=0 -> 4 KiB pages */
    char model[41]; for (int i = 0; i < 40; i++) model[i] = (char)idbuf[24 + i]; model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;     /* trim trailing spaces */

    /* Active Namespace List (CNS=2): use the first active NSID, not a hardcoded 1. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = 0; cmd.prp1 = (uint64_t)(uintptr_t)idbuf; cmd.cdw10 = 2;  /* CNS=2 */
    g_nsid = (nvme_submit(&g_admin, &cmd) == 0 && *(uint32_t *)idbuf) ? *(uint32_t *)idbuf : 1;

    /* Identify Namespace (chosen NSID) -> capacity + LBA size. */
    memset(idbuf, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06; cmd.nsid = g_nsid; cmd.prp1 = (uint64_t)(uintptr_t)idbuf; cmd.cdw10 = 0; /* CNS=0 */
    if (nvme_submit(&g_admin, &cmd) != 0) { kprintf("[nvme] identify ns failed\n"); pmm_free((uint64_t)(uintptr_t)idbuf); nvme_free_all(); return -1; }
    g_cap = *(uint64_t *)idbuf;                          /* NSZE */
    uint8_t flbas = idbuf[26] & 0xF;
    uint8_t lbads = idbuf[128 + flbas * 4 + 2];          /* LBAF[flbas].LBADS */
    g_lba = 1u << lbads;
    pmm_free((uint64_t)(uintptr_t)idbuf);                /* done with the identify buffer */
    if (g_lba != 512) { kprintf("[nvme] unsupported lba size %d (need 512)\n", (int)g_lba); nvme_free_all(); return -1; }

    /* Create I/O Completion Queue (qid 1). */
    g_io.cq = (struct nvme_cqe *)(uintptr_t)pmm_alloc();
    if (!g_io.cq) { nvme_free_all(); return -1; }
    memset(g_io.cq, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x05; cmd.prp1 = (uint64_t)(uintptr_t)g_io.cq;
    cmd.cdw10 = ((IO_DEPTH - 1) << 16) | 1;              /* qsize-1 | qid=1 */
    cmd.cdw11 = 1;                                       /* PC=1, IEN=0 (polled) */
    if (nvme_submit(&g_admin, &cmd) != 0) { kprintf("[nvme] create io cq failed\n"); nvme_free_all(); return -1; }

    /* Create I/O Submission Queue (qid 1, bound to cqid 1). */
    g_io.sq = (struct nvme_sqe *)(uintptr_t)pmm_alloc();
    if (!g_io.sq) { nvme_free_all(); return -1; }
    memset(g_io.sq, 0, 4096);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x01; cmd.prp1 = (uint64_t)(uintptr_t)g_io.sq;
    cmd.cdw10 = ((IO_DEPTH - 1) << 16) | 1;             /* qsize-1 | qid=1 */
    cmd.cdw11 = (1u << 16) | 1u;                        /* CQID=1 | PC=1 */
    if (nvme_submit(&g_admin, &cmd) != 0) { kprintf("[nvme] create io sq failed\n"); nvme_free_all(); return -1; }

    g_io.depth = IO_DEPTH; g_io.sq_tail = 0; g_io.cq_head = 0; g_io.cq_phase = 1; g_io.cid = 0;
    g_io.sq_db = g_regs + 0x1000 + 2 * stride;
    g_io.cq_db = g_regs + 0x1000 + 3 * stride;

    g_prp_list = (uint64_t *)(uintptr_t)pmm_alloc();    /* PRP-list page for large DMA */
    if (!g_prp_list) { nvme_free_all(); return -1; }

    g_ready = 1;
    kprintf("[nvme] up (slot %d %x:%x '%s') ns=%d lba=%d cap=%u sectors maxxfer=%d\n",
            dev.slot, dev.vendor, dev.device, model, (int)g_nsid, (int)g_lba,
            (unsigned)g_cap, (int)g_max_sectors);
    return 0;
}

/* Read/write `count` 512-byte sectors at `lba`, DMAing DIRECTLY from the caller's
 * identity-mapped buffer (kernel buffers: virt==phys) via PRP1 (+PRP2 page, or a
 * PRP list for >2 pages). No bounce buffer, no per-I/O memcpy. Each command moves
 * up to min(MDTS, one PRP-list page worth) sectors; large requests loop. */
static int nvme_io(int write, uint64_t lba, uint32_t count, void *buf)
{
    if (!g_ready) return -1;
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count;
        if (n > g_max_sectors) n = g_max_sectors;
        if (n > 2048) n = 2048;                         /* keep PRP list within one page */
        uint64_t addr  = (uint64_t)(uintptr_t)p;
        uint64_t bytes = (uint64_t)n * 512;
        uint64_t first = addr & ~0xFFFULL;
        uint64_t last  = (addr + bytes - 1) & ~0xFFFULL;
        int npages = (int)((last - first) / 0x1000) + 1;

        struct nvme_sqe cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0  = write ? 0x01 : 0x02;               /* Write : Read */
        cmd.nsid  = g_nsid;
        cmd.prp1  = addr;
        if (npages == 1)      cmd.prp2 = 0;
        else if (npages == 2) cmd.prp2 = first + 0x1000;
        else {                                          /* PRP list: pages 1..npages-1 */
            for (int i = 1; i < npages; i++) g_prp_list[i - 1] = first + (uint64_t)i * 0x1000;
            cmd.prp2 = (uint64_t)(uintptr_t)g_prp_list;
        }
        cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);      /* SLBA low */
        cmd.cdw11 = (uint32_t)(lba >> 32);             /* SLBA high */
        cmd.cdw12 = n - 1;                             /* NLB (0-based) */
        if (nvme_submit(&g_io, &cmd) != 0) return -1;
        p += bytes; lba += n; count -= n;
    }
    return 0;
}

int nvme_read(uint32_t lba, uint8_t count, void *buf)        { return nvme_io(0, lba, count, buf); }
int nvme_write(uint32_t lba, uint8_t count, const void *buf) { return nvme_io(1, lba, count, (void *)buf); }
