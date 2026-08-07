/* AHCI 1.x host bus adapter -- SATA disks on real hardware.
 *
 * Why this exists: every other block driver in this tree found its disk because
 * QEMU was told to hand us one specific device. virtio-blk needs a hypervisor;
 * NVMe needs a machine new enough to have one; IDE PIO is a compatibility mode
 * chipsets are dropping. AHCI is what an actual x86 box presents its SATA disks
 * through, and it ships under dozens of vendor:device IDs -- so it is matched by
 * PCI CLASS (01/06/01), never by ID.
 *
 * Scope: one command at a time per port (slot 0 only), polled, no NCQ, no
 * interrupts, no port multipliers, no ATAPI. That matches how logitfs uses the
 * block layer (strictly synchronous bread/bwrite) and it is the same shape as
 * virtio_blk.c and nvme.c, so the waiting discipline below is the one the rest
 * of the kernel already assumes.
 *
 * DMA memory: one 4 KiB page per port, identity-mapped like every kernel page
 * here, laid out to satisfy AHCI's alignment rules by construction --
 *   +0x000  command list   (1 KiB, needs 1 KiB alignment)
 *   +0x400  received FIS   (256 B, needs 256 B alignment)
 *   +0x800  command table  (128 B header + PRDT, needs 128 B alignment)
 * A page is 4 KiB-aligned, so all three fall out for free and no allocator with
 * an alignment parameter is needed.
 */
#include <stdint.h>
#include <stddef.h>
#include "ahci.h"
#include "blkdev.h"
#include "ata.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "pit.h"
#include "kprintf.h"

/* The device model (c/drivers/core/driver.h) landed while this driver was being
 * written, and it is the right way to find an AHCI controller: match on
 * class/subclass/prog-if, get a mapped BAR back, never scan.
 *
 * The guard is here because the two arrived in the same working tree from
 * different directions, and a checkout of this file WITHOUT that header must
 * still build a kernel that boots -- the block layer is on the path of every
 * other agent's boot, so it cannot be the thing that fails to compile. Delete
 * the #else branch, and this guard with it, once driver.h is unconditionally
 * present; the class triple is identical on both sides, so nothing else moves.
 *
 * Note this uses dev_find_class() directly rather than DRIVER_DECLARE + probe:
 * dev_probe_all() runs after smp_init(), which is long after vfs_mount(), and
 * the root disk has to exist before the root filesystem is mounted. A driver
 * that binds through the probe pass cannot be the one holding the root. */
#if defined(__has_include)
#  if __has_include("driver.h")
#    define AHCI_DEVICE_MODEL 1
#    include "driver.h"
#  endif
#endif

void *memset(void *, int, size_t);

/* --- HBA global registers --- */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28

#define GHC_HR     (1u << 0)    /* HBA reset */
#define GHC_IE     (1u << 1)
#define GHC_AE     (1u << 31)   /* AHCI enable */

#define CAP_S64A   (1u << 31)
#define CAP_SSS    (1u << 27)   /* staggered spin-up supported */

#define BOHC_BOS   (1u << 0)    /* BIOS owned semaphore */
#define BOHC_OOS   (1u << 1)    /* OS owned semaphore */
#define BOHC_BB    (1u << 4)    /* BIOS busy */

/* --- per-port registers, at 0x100 + port*0x80 --- */
#define P_CLB      0x00
#define P_CLBU     0x04
#define P_FB       0x08
#define P_FBU      0x0C
#define P_IS       0x10
#define P_IE       0x14
#define P_CMD      0x18
#define P_TFD      0x20
#define P_SIG      0x24
#define P_SSTS     0x28
#define P_SCTL     0x2C
#define P_SERR     0x30
#define P_SACT     0x34
#define P_CI       0x38

#define CMD_ST     (1u << 0)
#define CMD_SUD    (1u << 1)    /* spin-up device */
#define CMD_POD    (1u << 2)    /* power on device */
#define CMD_FRE    (1u << 4)    /* FIS receive enable */
#define CMD_FR     (1u << 14)   /* FIS receive running */
#define CMD_CR     (1u << 15)   /* command list running */

#define TFD_ERR    (1u << 0)
#define TFD_DRQ    (1u << 3)
#define TFD_BSY    (1u << 7)

#define IS_TFES    (1u << 30)   /* task file error status */

#define SIG_SATA   0x00000101u
#define SIG_ATAPI  0xEB140101u
#define SIG_SEMB   0xC33C0101u
#define SIG_PM     0x96690101u

/* ATA command set (the same commands ata.c issues over the legacy taskfile;
 * AHCI is a different transport for them, not a different command set). */
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_READ_DMA       0xC8
#define ATA_WRITE_DMA      0xCA
#define ATA_FLUSH_EXT      0xEA
#define ATA_FLUSH          0xE7
#define ATA_IDENTIFY       0xEC

#define AHCI_MAX_PORTS  32
#define AHCI_MAX_DISKS  8
#define AHCI_MAX_HBA    4               /* controllers brought up; a box has 1-2 */
#define AHCI_PRDT       8               /* 8 x 4 MiB = 32 MiB per command */
#define AHCI_PRD_MAX    0x400000u       /* DBC is 22 bits: 4 MiB per PRD entry */

/* Two independent bounds on every wait. The spin cap is the one that holds
 * during init, when kmain still runs with IF=0 and the PIT tick therefore does
 * not advance; the millisecond deadline is the one that holds during a command,
 * where we deliberately run with IF=1 (see ahci_cmd) and the tick is live.
 * Neither alone is enough: a pure spin count has no defensible unit on a TCG
 * host whose speed varies by two orders of magnitude, and a pure ms deadline
 * never fires when the clock is frozen. */
#define SPIN_INIT   20000000L
#define SPIN_CMD    200000000L

struct ahci_port {
    int      index;
    volatile uint8_t *reg;          /* port register block */
    uint8_t *dma;                   /* the 4 KiB page (identity mapped: virt == phys) */
    uint64_t nsectors;
    int      lba48;
    char     model[41];
    char     name[8];               /* "ahci0" ... */
};

static volatile uint8_t *g_abar;
static struct ahci_port  g_ports[AHCI_MAX_DISKS];
static int               g_ndisks;

static inline uint32_t r32(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static inline void     w32(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void     barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

int ahci_disk_count(void) { return g_ndisks; }

/* A crude spin delay. Used only where the spec asks for a settling period with
 * no register to watch (staggered spin-up), and only during init, where the PIT
 * tick is not yet advancing and there is no better clock to wait on. */
static void spin_delay(long n) { for (volatile long i = 0; i < n; i++) { } }

/* Wait for `mask` to read as zero in a register, or give up. */
static int wait_clear(volatile uint8_t *base, int off, uint32_t mask, long spins, uint32_t ms)
{
    uint64_t deadline = timer_ms() + ms;
    for (long i = 0; i < spins; i++) {
        if ((r32(base, off) & mask) == 0) return 0;
        if ((i & 0xFFFF) == 0xFFFF && timer_ms() > deadline) return -1;
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Port start/stop
 *
 * The order is not a style choice: the spec requires the command engine to be
 * stopped and observed stopped (CR clear) before the command list address may
 * be changed, and the FIS engine likewise (FR clear) before the FIS base may.
 * Writing CLB while CR is still set is the classic way to get a controller that
 * DMAs into the page you just freed.
 * ------------------------------------------------------------------------ */
static int port_stop(struct ahci_port *p)
{
    uint32_t cmd = r32(p->reg, P_CMD);
    w32(p->reg, P_CMD, cmd & ~CMD_ST);
    if (wait_clear(p->reg, P_CMD, CMD_CR, SPIN_INIT, 600)) return -1;
    cmd = r32(p->reg, P_CMD);
    w32(p->reg, P_CMD, cmd & ~CMD_FRE);
    if (wait_clear(p->reg, P_CMD, CMD_FR, SPIN_INIT, 600)) return -1;
    return 0;
}

static void port_start(struct ahci_port *p)
{
    /* FRE before ST, and only once BSY/DRQ are clear: starting the command
     * engine against a device that still has a transfer in progress is how a
     * port ends up wedged with no error bit to explain it. */
    (void)wait_clear(p->reg, P_TFD, TFD_BSY | TFD_DRQ, SPIN_INIT, 1000);
    w32(p->reg, P_CMD, r32(p->reg, P_CMD) | CMD_FRE);
    w32(p->reg, P_CMD, r32(p->reg, P_CMD) | CMD_ST);
}

/* Clear the accumulated error registers and re-run the engines. Called between
 * command retries -- a task-file error leaves ST set but the port refusing to
 * make progress until SERR is acknowledged. */
static void port_recover(struct ahci_port *p)
{
    w32(p->reg, P_CMD, r32(p->reg, P_CMD) & ~CMD_ST);
    (void)wait_clear(p->reg, P_CMD, CMD_CR, SPIN_INIT, 600);
    w32(p->reg, P_SERR, r32(p->reg, P_SERR));   /* write-1-to-clear */
    w32(p->reg, P_IS,   r32(p->reg, P_IS));
    (void)wait_clear(p->reg, P_TFD, TFD_BSY | TFD_DRQ, SPIN_INIT, 1000);
    w32(p->reg, P_CMD, r32(p->reg, P_CMD) | CMD_ST);
}

/* --------------------------------------------------------------------------
 * Issuing a command
 * ------------------------------------------------------------------------ */

/* Build slot 0's command header + command table and run it to completion.
 *
 * `write` selects the H2D direction bit, `buf`/`bytes` describe the (identity
 * mapped, hence physically contiguous) data area, 0 bytes means a non-data
 * command such as FLUSH.
 *
 * Waiting discipline, and why it is this one: completions are produced by
 * QEMU's IO thread, which only runs when this vCPU yields, so a tight IF=0
 * busy-poll starves the very completion it is waiting for and always times out.
 * So interrupts are ENABLED across the poll. But the timer must not preempt us
 * mid-command -- a context switch here abandons a controller with CI still set
 * and a PRDT pointing at a buffer the next thread may reuse. g_ata_busy is the
 * flag c/kernel/cpu/interrupts.c already consults to skip schedule() for exactly
 * this reason (ata_busy()); AHCI shares it rather than adding a second one,
 * because it is the same claim -- an ATA-family transfer is in flight -- and
 * because interrupts.c is not this change's to edit. */
static int ahci_cmd_once(struct ahci_port *p, int write, uint8_t command,
                         uint64_t lba, uint32_t sectors, void *buf, uint32_t bytes)
{
    uint32_t *hdr = (uint32_t *)p->dma;                 /* command header, slot 0 */
    uint8_t  *tbl = p->dma + 0x800;                     /* command table */
    uint8_t  *fis = tbl;                                /* CFIS at table offset 0 */
    uint8_t  *prd = tbl + 0x80;

    if (wait_clear(p->reg, P_TFD, TFD_BSY | TFD_DRQ, SPIN_INIT, 2000)) return -1;

    memset(tbl, 0, 0x80 + AHCI_PRDT * 16);

    int nprd = 0;
    uint64_t addr = (uint64_t)(uintptr_t)buf;
    uint32_t left = bytes;
    while (left > 0 && nprd < AHCI_PRDT) {
        uint32_t chunk = left > AHCI_PRD_MAX ? AHCI_PRD_MAX : left;
        uint32_t *e = (uint32_t *)(prd + nprd * 16);
        e[0] = (uint32_t)addr;
        e[1] = (uint32_t)(addr >> 32);
        e[2] = 0;
        e[3] = chunk - 1;                               /* DBC: byte count minus one */
        addr += chunk; left -= chunk; nprd++;
    }
    if (left > 0) return -1;                            /* caller must chunk further */

    /* Host-to-device register FIS. */
    fis[0]  = 0x27;                                     /* FIS type: H2D register */
    fis[1]  = 0x80;                                     /* C: this is a command, not a control write */
    fis[2]  = command;
    fis[3]  = 0;                                        /* features */
    fis[4]  = (uint8_t)(lba);
    fis[5]  = (uint8_t)(lba >> 8);
    fis[6]  = (uint8_t)(lba >> 16);
    fis[7]  = 0x40;                                     /* device: LBA mode */
    fis[8]  = (uint8_t)(lba >> 24);
    fis[9]  = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[11] = 0;
    fis[12] = (uint8_t)(sectors);
    fis[13] = (uint8_t)(sectors >> 8);

    hdr[0] = (5u)                                       /* CFL: 5 dwords of FIS */
           | (write ? (1u << 6) : 0u)                   /* W: host to device */
           | ((uint32_t)nprd << 16);                    /* PRDTL */
    hdr[1] = 0;                                         /* PRDBC: device fills this in */
    hdr[2] = (uint32_t)(uintptr_t)tbl;
    hdr[3] = (uint32_t)((uint64_t)(uintptr_t)tbl >> 32);

    w32(p->reg, P_IS, r32(p->reg, P_IS));               /* clear stale status */
    barrier();

    uint64_t fl; __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    g_ata_busy++;
    __asm__ volatile ("sti");
    w32(p->reg, P_CI, 1u);                              /* issue slot 0 */

    int rc = -1;
    uint64_t deadline = timer_ms() + 8000;
    for (long i = 0; i < SPIN_CMD; i++) {
        uint32_t is = r32(p->reg, P_IS);
        if (is & IS_TFES) break;                        /* device reported a task-file error */
        if ((r32(p->reg, P_CI) & 1u) == 0) {
            rc = (r32(p->reg, P_TFD) & TFD_ERR) ? -1 : 0;
            break;
        }
        if ((i & 0xFFFF) == 0xFFFF && timer_ms() > deadline) break;
    }
    if (!(fl & 0x200)) __asm__ volatile ("cli");        /* restore the caller's IF */
    g_ata_busy--;

    /* A read must have moved every byte it asked for. Without this a short DMA
     * -- the shape a truncated or mis-programmed PRDT produces -- returns
     *  success with a buffer that is partly whatever was there before. */
    if (rc == 0 && !write && bytes && hdr[1] < bytes) rc = -1;
    return rc;
}

/* Retry the whole command, recovering the port in between.
 *
 * ata.c carries the same 8x loop and CLAUDE.md records why: under -smp TCG the
 * AP's framebuffer present contends the big QEMU lock and delays the device
 * thread past a bounded poll, which surfaces as a nondeterministic "file not
 * found" rather than as an I/O error. AHCI completions come off the same IO
 * thread under the same lock, so the same fragility applies and is designed for
 * up front rather than discovered as flakiness. The recovery between attempts
 * is the part ata.c does not need: a timed-out AHCI command leaves CI set and
 * SERR latched, and re-issuing into that state fails forever. */
static int ahci_cmd(struct ahci_port *p, int write, uint8_t command,
                    uint64_t lba, uint32_t sectors, void *buf, uint32_t bytes)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        if (ahci_cmd_once(p, write, command, lba, sectors, buf, bytes) == 0) return 0;
        port_recover(p);
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Block device ops
 * ------------------------------------------------------------------------ */

static int ahci_rw(void *ctx, int write, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = (struct ahci_port *)ctx;
    if (count == 0) return -1;
    if (lba >= p->nsectors || (uint64_t)count > p->nsectors - lba) return -1;

    while (count > 0) {
        /* Bound each command by the smallest of: what the PRDT can describe,
         * what the sector-count field can express, and (for a drive without
         * LBA48) what the 28-bit addressing can reach. */
        uint32_t n = count;
        uint32_t prd_max = (AHCI_PRDT * AHCI_PRD_MAX) / 512;
        if (n > prd_max) n = prd_max;
        if (p->lba48) { if (n > 65535) n = 65535; }
        else {
            if (n > 255) n = 255;
            if (lba + n > (1u << 28)) return -1;
        }
        uint8_t cmd = p->lba48 ? (write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT)
                               : (write ? ATA_WRITE_DMA     : ATA_READ_DMA);
        if (ahci_cmd(p, write, cmd, lba, n, buf, n * 512u) != 0) return -1;
        buf = (uint8_t *)buf + (uint64_t)n * 512;
        lba += n; count -= n;
    }
    return 0;
}

static int ahci_op_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return ahci_rw(ctx, 0, lba, count, buf);
}
static int ahci_op_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    return ahci_rw(ctx, 1, lba, count, (void *)buf);
}
/* FLUSH CACHE (EXT) -- the barrier the journal's ordering rests on. A SATA disk
 * has a writeback cache by default, so a completed WRITE DMA means the drive
 * accepted the data, not that a platter holds it. */
static int ahci_op_flush(void *ctx)
{
    struct ahci_port *p = (struct ahci_port *)ctx;
    return ahci_cmd(p, 0, p->lba48 ? ATA_FLUSH_EXT : ATA_FLUSH, 0, 0, NULL, 0);
}

static const struct blk_ops ahci_ops = { ahci_op_read, ahci_op_write, ahci_op_flush };

/* --------------------------------------------------------------------------
 * Discovery
 * ------------------------------------------------------------------------ */

/* An AHCI controller, however it was located. */
struct ahci_hba {
    uint16_t vendor, device;
    uint8_t  bus, slot, func;
    uint64_t abar;                  /* BAR5, mapped */
};

/* AHCI is PCI class 01 / subclass 06 / prog-if 01, and matching on THAT rather
 * than on a vendor:device pair is the entire point: these controllers ship
 * under dozens of IDs (Intel alone has scores) and a driver keyed to any of
 * them finds a disk on exactly one machine. prog-if 01 is "AHCI 1.0"; 01/06/00
 * is a vendor-specific SATA interface that does not answer to these registers,
 * so it is deliberately not matched. */
#define AHCI_CLASS     0x01
#define AHCI_SUBCLASS  0x06
#define AHCI_PROGIF    0x01

#ifdef AHCI_DEVICE_MODEL

/* Select the `nth` matching controller (counting matches, not successes, so the
 * index stays stable when one of them turns out to have no usable ABAR). Sets
 * out->abar to 0 in that case rather than skipping, so the caller can say which
 * controller it declined and why. Returns -1 when there is no nth match. */
static int ahci_find(struct ahci_hba *out, int nth)
{
    for (struct device *d = dev_find_class(AHCI_CLASS, AHCI_SUBCLASS, NULL); d;
         d = dev_find_class(AHCI_CLASS, AHCI_SUBCLASS, d)) {
        if (d->prog_if != AHCI_PROGIF) continue;
        if (nth-- > 0) continue;
        dev_enable(d, 1);                       /* MEM decode + bus master, for DMA */
        out->vendor = d->vendor; out->device = d->device;
        out->bus = d->bus; out->slot = d->slot; out->func = d->func;
        out->abar = dev_bar_map(d, 5);          /* ABAR is BAR5, always */
        return 0;
    }
    return -1;
}

#else   /* transitional: no device model in this checkout */

/* Same match key, done by hand. Every function is examined, not just function
 * 0: on real chipsets the AHCI controller is very often a non-zero function of
 * a multifunction device (00:1f.2 on Intel), which a func-0-only scan misses.
 * Bus 0 only -- a controller behind a PCI-to-PCI bridge needs the bridge
 * traversal the device model does, which is the other reason this branch is
 * the one that goes away. */
static int ahci_find(struct ahci_hba *out, int nth)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id0 = pci_cfg_read(0, slot, 0, 0x00);
        if ((id0 & 0xFFFF) == 0xFFFF) continue;
        uint8_t hdr = (uint8_t)((pci_cfg_read(0, slot, 0, 0x0C) >> 16) & 0xFF);
        uint8_t nfunc = (hdr & 0x80) ? 8 : 1;           /* bit 7: multifunction */
        for (uint8_t func = 0; func < nfunc; func++) {
            uint32_t id = pci_cfg_read(0, slot, func, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint32_t cls = pci_cfg_read(0, slot, func, 0x08) >> 8;
            if (cls != ((AHCI_CLASS << 16) | (AHCI_SUBCLASS << 8) | AHCI_PROGIF)) continue;
            if (nth-- > 0) continue;
            uint32_t bar5 = pci_cfg_read(0, slot, func, 0x24) & ~(uint32_t)0xF;
            if (bar5 == 0xFFFFFFF0u) bar5 = 0;
            if (bar5) {
                uint32_t cmd = pci_cfg_read(0, slot, func, 0x04);
                pci_cfg_write(0, slot, func, 0x04, cmd | 0x02 | 0x04);   /* MEM + bus master */
                vmm_map_range(bar5, bar5, 0x2000, VMM_WRITABLE | VMM_NOCACHE);
            }
            out->bus = 0; out->slot = slot; out->func = func;
            out->vendor = id & 0xFFFF; out->device = id >> 16;
            out->abar = bar5;
            return 0;
        }
    }
    return -1;
}

#endif

/* IDENTIFY DEVICE: capacity, LBA48 support and the model string, read from the
 * drive rather than assumed. The capacity is what bounds every later request,
 * so getting it from the device is what stops a partition table that claims
 * more sectors than exist from being believed. */
static int port_identify(struct ahci_port *p)
{
    static uint16_t id[256] __attribute__((aligned(64)));   /* identity mapped DMA target */
    memset(id, 0, sizeof id);
    if (ahci_cmd(p, 0, ATA_IDENTIFY, 0, 0, id, sizeof id) != 0) return -1;

    p->lba48 = (id[83] & (1u << 10)) ? 1 : 0;
    uint64_t caps = p->lba48
        ? ((uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48))
        : ((uint64_t)id[60] | ((uint64_t)id[61] << 16));
    if (caps == 0 && p->lba48) caps = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    p->nsectors = caps;

    /* Words 27..46, ASCII, each word stored big-endian on the wire. */
    for (int i = 0; i < 20; i++) {
        p->model[i * 2]     = (char)(id[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    p->model[40] = 0;
    for (int i = 39; i >= 0 && (p->model[i] == ' ' || p->model[i] == 0); i--) p->model[i] = 0;
    return p->nsectors ? 0 : -1;
}

static const char *sig_name(uint32_t sig)
{
    switch (sig) {
    case SIG_SATA:  return "SATA disk";
    case SIG_ATAPI: return "SATAPI";
    case SIG_SEMB:  return "enclosure";
    case SIG_PM:    return "port multiplier";
    default:        return "unknown";
    }
}

/* Bring up one controller: BIOS handoff, AHCI enable, then every implemented
 * port. Returns the number of SATA disks registered off it. */
static int ahci_bring_up(const struct ahci_hba *dev)
{
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)dev->abar;
    g_abar = abar;

    /* BIOS/OS handoff: on real firmware the BIOS owns the controller until we
     * ask for it, and touching GHC before it lets go is how a machine hangs at
     * this exact line. Skipped silently when the capability is absent (QEMU). */
    if (r32(abar, HBA_CAP2) & 1u) {
        w32(abar, HBA_BOHC, r32(abar, HBA_BOHC) | BOHC_OOS);
        (void)wait_clear(abar, HBA_BOHC, BOHC_BOS, SPIN_INIT, 2000);
        (void)wait_clear(abar, HBA_BOHC, BOHC_BB,  SPIN_INIT, 2000);
    }

    w32(abar, HBA_GHC, r32(abar, HBA_GHC) | GHC_AE);
    w32(abar, HBA_GHC, r32(abar, HBA_GHC) & ~GHC_IE);   /* we poll; no HBA interrupts */

    uint32_t cap = r32(abar, HBA_CAP);
    uint32_t pi  = r32(abar, HBA_PI);
    uint32_t vs  = r32(abar, HBA_VS);
    int nports = (int)(cap & 0x1F) + 1;
    int ncs    = (int)((cap >> 8) & 0x1F) + 1;

    kprintf("[ahci] %x:%x at %02x:%02x.%d abar=%x ver=%d.%d ports=%d impl=%x slots=%d addr64=%s\n",
            dev->vendor, dev->device, (int)dev->bus, (int)dev->slot, (int)dev->func,
            (unsigned)dev->abar,
            (int)(vs >> 16), (int)((vs >> 8) & 0xFF), nports, pi, ncs,
            (cap & CAP_S64A) ? "yes" : "no");

    int found = 0;
    for (int i = 0; i < AHCI_MAX_PORTS && i < 32; i++) {
        if (!(pi & (1u << i))) continue;                /* not implemented */
        volatile uint8_t *pr = abar + 0x100 + i * 0x80;

        /* Staggered spin-up: on hardware that supports it a port can be dark
         * until asked. Costs nothing where it is not needed. */
        if (cap & CAP_SSS) {
            w32(pr, P_CMD, r32(pr, P_CMD) | CMD_SUD | CMD_POD);
            spin_delay(2000000L);
        }

        uint32_t ssts = r32(pr, P_SSTS);
        uint32_t det = ssts & 0xF, ipm = (ssts >> 8) & 0xF;
        if (det != 3) continue;                         /* no device, or PHY not communicating */
        if (ipm != 1) continue;                         /* not in active power state */

        uint32_t sig = r32(pr, P_SIG);
        if (sig != SIG_SATA) {
            /* Reported, not mounted: an ATAPI drive answers on the same port
             * registers and would accept READ DMA EXT with results nobody
             * wants. Distinguishing by signature is the point of reading it. */
            kprintf("[ahci] port %d: %s (sig=%08x) -- skipped, not a SATA disk\n",
                    i, sig_name(sig), sig);
            continue;
        }
        if (g_ndisks >= AHCI_MAX_DISKS) {
            kprintf("[ahci] port %d: SATA disk ignored, only %d supported\n", i, AHCI_MAX_DISKS);
            continue;
        }

        struct ahci_port *p = &g_ports[g_ndisks];
        p->index = i;
        p->reg   = pr;
        p->dma   = (uint8_t *)(uintptr_t)pmm_alloc();
        if (!p->dma) { kprintf("[ahci] port %d: out of memory\n", i); continue; }
        memset(p->dma, 0, 4096);

        if (port_stop(p) != 0) {
            kprintf("[ahci] port %d: engines would not stop (cmd=%x) -- skipped\n", i, r32(pr, P_CMD));
            pmm_free((uint64_t)(uintptr_t)p->dma);
            continue;
        }
        w32(pr, P_CLB,  (uint32_t)(uintptr_t)p->dma);
        w32(pr, P_CLBU, (uint32_t)((uint64_t)(uintptr_t)p->dma >> 32));
        w32(pr, P_FB,   (uint32_t)((uintptr_t)p->dma + 0x400));
        w32(pr, P_FBU,  (uint32_t)(((uint64_t)(uintptr_t)p->dma + 0x400) >> 32));
        w32(pr, P_SERR, r32(pr, P_SERR));
        w32(pr, P_IS,   r32(pr, P_IS));
        w32(pr, P_IE,   0);
        port_start(p);

        if (port_identify(p) != 0) {
            kprintf("[ahci] port %d: IDENTIFY failed (tfd=%x serr=%x) -- skipped\n",
                    i, r32(pr, P_TFD), r32(pr, P_SERR));
            (void)port_stop(p);
            pmm_free((uint64_t)(uintptr_t)p->dma);
            continue;
        }

        p->name[0] = 'a'; p->name[1] = 'h'; p->name[2] = 'c'; p->name[3] = 'i';
        p->name[4] = (char)('0' + g_ndisks); p->name[5] = 0;
        kprintf("[ahci] port %d: SATA disk '%s' %u sectors (%u MiB) lba48=%s\n",
                i, p->model, (unsigned)p->nsectors, (unsigned)(p->nsectors / 2048),
                p->lba48 ? "yes" : "no");
        blk_register(p->name, &ahci_ops, p, p->nsectors);
        g_ndisks++;
        found++;
    }

    if (!found) kprintf("[ahci] controller up, no SATA disks attached\n");
    return found;
}

/* Every AHCI controller, not just the first.
 *
 * A machine with a chipset AHCI and an add-in SATA card has two, and taking the
 * first and stopping means the disks on the other one do not exist -- which on
 * a box that boots off the add-in card means the OS does not boot. There is no
 * cost to the loop: on a machine with one controller the second lookup simply
 * finds nothing. */
int ahci_init(void)
{
    int total = 0;
    for (int nth = 0; nth < AHCI_MAX_HBA; nth++) {
        struct ahci_hba dev;
        if (ahci_find(&dev, nth) != 0) break;           /* no more: not an error */
        if (!dev.abar) {
            kprintf("[ahci] %x:%x at %02x:%02x.%d has no usable ABAR -- skipped\n",
                    dev.vendor, dev.device, (int)dev.bus, (int)dev.slot, (int)dev.func);
            continue;
        }
        total += ahci_bring_up(&dev);
    }
    return total;
}
