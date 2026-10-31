/* AHCI SATA disks: early polling, late MSI, independent NCQ data slots.
 *
 * Correction 2026-11-01, kept beside the old claims: this file used to say
 * "NCQ: WHY THERE IS STILL ONLY SLOT 0" and "WHY THIS IS STILL A POLL".
 * Those were accurate with blkdev's one-in-flight interlock and sequential
 * bcache_sync. The block layer now opts capable media into bounded queues and
 * bcache_sync supplies independent dirty blocks in batches of sixteen. Root
 * mount still polls; ahci_irq_attach runs only after dev_probe_all and SMP.
 * MSI delivery is tested, and absent delivery automatically retains polling.
 *
 * Do NOT split requests merely to fill tags. Historical QEMU measurements
 * (2026-08-20, f8d2ca4): 741 eight-sector commands reading a 3 MB app cost
 * 97.6 ms; nine coalesced commands cost 1.35 ms. Each command carried a fixed
 * charge (~110 us for 8 sectors, 288 us for 2048). DMA_MAX_MAPPING now bounds
 * each chunk to 1 MiB, independently of AHCI's 8 x 4 MiB PRDT capacity.
 * The queue is for independent requests, not manufactured fragments.
 *
 * NCQ errors abort a queue, not one request. READ LOG EXT page 10h attributes
 * the failed tag; DMA tokens for already completed siblings must be consumed
 * before device-wide quiescence invalidates them. The ISR latches status only;
 * request/DMA ownership transitions belong to the medium gate's poll path.
 * HLT never hands the CPU to another thread while g_ata_busy is raised, and
 * only the MSI destination CPU may halt: APs otherwise pay a whole local tick.
 *
 * Metadata: command list +0x000 (1 KiB alignment), receive FIS +0x400 (256),
 * command tables +0x800+slot*0x100 (128-byte header + eight 16-byte PRDs).
 * CAP.NCS+1 tables are allocated together and rounded to whole pages; CPU
 * aliases and DMA addresses are separate. One slot is reserved for internal
 * nonqueued commands. No ATAPI, port multipliers, read-ahead or swap concurrency.
 *
 * Sources: AHCI 1.3.1; Linux v6.12 drivers/ata/libahci.c, libata-core.c and
 * libata-eh.c (functions cited at the hardware sequences below):
 * https://github.com/torvalds/linux/tree/v6.12/drivers/ata
 * Model and QEMU evidence only: not verified on X79-VG4 hardware.
 */
#include "ktime.h"
#include <stdint.h>
#include <stddef.h>
#include "ahci.h"
#include "blkdev.h"
#include "ata.h"
#include "pci.h"
#include "vmm.h"
#include "dma.h"
#include "panic.h"
#include "pit.h"
#include "kprintf.h"
#include "lapic.h"

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

#ifndef memset
void *memset(void *, int, size_t);
#endif

/* Only privileged CPU operations and MMIO are substituted in the host model.
 * The command, DMA ownership and recovery state machines remain identical. */
#ifdef AHCI_HOSTTEST
#define AHCI_FLAGS(flags) ((flags) = ahci_host_flags())
#define AHCI_STI() ahci_host_sti()
#define AHCI_CLI() ahci_host_cli()
#define AHCI_PAUSE() ahci_host_pause()
#define AHCI_HALT() ahci_host_halt()
#else
#define AHCI_FLAGS(flags) __asm__ volatile ("pushfq; pop %0" : "=r"(flags) :: "memory")
#define AHCI_STI() __asm__ volatile ("sti" ::: "memory")
#define AHCI_CLI() __asm__ volatile ("cli" ::: "memory")
#define AHCI_PAUSE() __asm__ volatile ("pause" ::: "memory")
/* STI's interrupt shadow makes the adjacent HLT atomic against a delivered MSI
 * (Intel SDM Vol. 2, STI). Splitting these asm statements loses that property. */
#define AHCI_HALT() __asm__ volatile ("sti; hlt" ::: "memory")
#endif

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
#define CAP_SNCQ   (1u << 30)   /* native command queuing supported */
#define CAP_SSS    (1u << 27)   /* staggered spin-up supported */
#define CAP_SCLO   (1u << 24)   /* AHCI 1.3.1 3.1.1: command-list override */

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
#define CMD_CLO    (1u << 3)    /* AHCI 1.3.1 3.3.7: clear HBA BSY/DRQ shadow, not device state */
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
/* AHCI command tables need 128-byte alignment (4.2.3); 128-byte CFIS/ACMD
 * header plus eight 16-byte PRDs fits in 256 bytes. Slot zero stays at +0x800. */
#define AHCI_CT_STRIDE 0x100
#define AHCI_CT_OFF(slot) (0x800 + (slot) * AHCI_CT_STRIDE)
/* AHCI 1.3.1 3.3.6: DHRS, PSS, SDBS, OFE, INFE, IFE, HBDE, HBFE, TFEE.
 * This bring-up mask is intentionally narrower than Linux v6.12 ahci.h
 * DEF_PORT_IRQ (which also covers hotplug/unknown FIS); it is not its value. */
#define AHCI_PXIE_DEFAULT (0x1u|0x2u|0x8u|(1u<<24)|(1u<<26)|(1u<<27)|(1u<<28)|(1u<<29)|(1u<<30))
/* Fatal transport errors: interface fatal, host-bus data and host-bus fatal;
 * taskfile ERR is separate. Linux v6.12 libahci.c ahci_error_intr. */
#define AHCI_IS_FATAL ((1u << 27) | (1u << 28) | (1u << 29))
#define AHCI_RC_TFES 1
#define AHCI_RC_TIMEOUT 2
#define AHCI_RC_FATAL 3
#define AHCI_SLOT_FREE 0
#define AHCI_SLOT_ACTIVE 1
#define AHCI_SLOT_DONE 2
#define AHCI_SLOT_FAILED 3

/* Two independent bounds on every wait. The spin cap is the one that holds
 * during init, when kmain still runs with IF=0 and the PIT tick therefore does
 * not advance; the millisecond deadline is the one that holds during a command,
 * where we deliberately run with IF=1 (see ahci_cmd) and the tick is live.
 * Neither alone is enough: a pure spin count has no defensible unit on a TCG
 * host whose speed varies by two orders of magnitude, and a pure ms deadline
 * never fires when the clock is frozen. */
#define SPIN_INIT   20000000L
#define SPIN_CMD    200000000L

/* One device command. The block layer's request is chunked into a sequence of
 * these; IDENTIFY is one on its own. It lives in the PORT rather than in the
 * caller's frame because a command now outlives the call that issued it -- a
 * retry after a task-file error re-issues the SAME spec, and the poll that
 * performs the retry may be several returns away from the submit. */
struct ahci_cmdspec {
    uint8_t  command;
    int      write;
    uint64_t lba;
    uint32_t sectors;
    void    *buf;
    uint32_t bytes;
    uint8_t ncq;
    uint8_t tag;
};

#define AHCI_RETRIES 8

struct ahci_slot {
    struct ahci_cmdspec cur;
    struct dma_mapping *mapping;
    uint64_t map_token;
    struct blk_req *req;
    uint64_t deadline;
    uint8_t attempt, state;
    uint8_t timeouts;
    uint8_t stale_error;
    uint32_t irq_start;
};

struct ahci_stats {
    uint32_t spurious, poll_done, max_inflight, errors, recoveries, resets;
};

/* One vector per HBA. The ISR never dereferences blk_req or streaming maps.
 * irq_cpu is the MSI destination chosen by pci_msi.c, not the waiting CPU. */
struct ahci_ctl {
    struct device *dev;
    volatile uint8_t *reg;
    uint32_t pi, irq_cpu;
    int irq_armed, attach_tried;
};

struct ahci_ncq_err {
    uint8_t tag, status, error;
    uint64_t lba;
};

struct ahci_port {
    struct dma_device dma_dev;
    struct dma_buffer *metadata;
    struct device *controller;
    struct blkdev *block;

    int      index;
    volatile uint8_t *reg;          /* port register block */
    /* Previously "the 4 KiB page": metadata now spans whole pages for every
     * CAP.NCS slot (12 KiB at 32 slots, 4 KiB at one). This is the CPU alias;
     * command addresses come from metadata->dma, not an identity-map assumption. */
    uint8_t *dma;
    uint64_t nsectors;
    int      lba48;
    char     model[41];
    char     name[8];               /* "ahci0" ... */

    struct ahci_ctl *ctl;
    uint32_t cap, nslots, qdepth, ncq_words, active_mask, ncq_mask;
    uint32_t is_latched, irq_events, irq_seen;
    /* Atomic bitmap of slots the ISR actually observed finished. A port-wide
     * IRQ generation cannot attribute completion when siblings finish apart. */
    uint32_t irq_completed;
    /* IRQ collection never takes the medium gate. This separate short gate
     * couples epoch validation, latch publication and W1C with recovery's
     * discard boundary; it is never held across a stop, DMA work or polling. */
    io_lock_t irq_gate;
    uint64_t error_epoch;
    unsigned ncq_recoveries;
    int recovering;
    struct ahci_stats st;
    struct ahci_slot slot[32];

};

static volatile uint8_t *g_abar;
static struct ahci_port  g_ports[AHCI_MAX_DISKS];
static int               g_ndisks;
static struct ahci_ctl g_ctls[AHCI_MAX_HBA];
static unsigned g_nctls;

/* Keep capability policy with this driver; no upper layer guesses whether an
 * HBA can address 64-bit PRDs or command-list/FIS structures. */
static int ahci_dma_init(struct ahci_port *p, uint32_t cap)
{
    dma_device_init(&p->dma_dev, "ahci", (cap & CAP_S64A) ? DMA_MASK_64 : DMA_MASK_32);
    p->dma_dev.max_segments = AHCI_PRDT;
    p->dma_dev.max_segment = AHCI_PRD_MAX;
    p->cap = cap;
    p->nslots = ((cap >> 8) & 31) + 1;
    p->qdepth = 1;
    /* NCS=31: 0x2800 rounds to 12 KiB; NCS=0: 0x900 stays one page.
     * AHCI 1.3.1 3.1.1 NCS, 4.2 command list/table alignment. */
    size_t bytes = (AHCI_CT_OFF(p->nslots) + 4095u) & ~(size_t)4095u;
    p->metadata = dma_alloc_coherent(&p->dma_dev, bytes, 4096, 0);
    p->dma = p->metadata ? p->metadata->cpu : NULL;
    return p->dma ? 0 : -1;
}

/* Keep register access widths fixed; AHCI register semantics are 32-bit. */
static inline uint32_t r32(volatile uint8_t *b, int o)
{
#ifdef AHCI_HOSTTEST
    return ahci_host_read(b, o);
#else
    return *(volatile uint32_t *)(b + o);
#endif
}

/* The model implements W1S/W1C; normal RAM assignments cannot model AHCI. */
static inline void w32(volatile uint8_t *b, int o, uint32_t v)
{
#ifdef AHCI_HOSTTEST
    ahci_host_write(b, o, v);
#else
    *(volatile uint32_t *)(b + o) = v;
#endif
}

/* Publish command memory before ringing PxSACT/PxCI (AHCI 1.3.1 5.3). */
static inline void barrier(void)
{
#ifdef AHCI_HOSTTEST
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
    __asm__ volatile ("mfence" ::: "memory");
#endif
}

/* Intersect HBA slots with IDENTIFY queue depth. One internal slot is reserved
 * even when the drive supports 32 tags: READ LOG must not overwrite a data FIS.
 * Source: Linux v6.12 libata-core.c ata_dev_config_ncq, ata.h ata_id_queue_depth
 * and ata_id_has_ncq; https://github.com/torvalds/linux/tree/v6.12/drivers/ata . */
static unsigned ahci_ncq_depth(uint32_t cap, uint32_t ncq_words)
{
    unsigned slots = ((cap >> 8) & 31) + 1;
    unsigned word76 = ncq_words >> 16;
#ifdef AHCI_NONCQ
    return 0;
#endif
    if (!(cap & CAP_SNCQ) || slots < 2) return 0;
#ifndef AHCI_NEGCTL_NCQ_IGNORE_W76
    if (!word76 || word76 == 0xffff || !(word76 & 0x100)) return 0;
#else
    (void)word76;
#endif
    unsigned depth = (ncq_words & 31) + 1;
    if (depth < 2) return 0;
#ifdef AHCI_NEGCTL_NCQ_IGNORE_W75
    depth = 32;
#endif
    if (depth > slots - 1) depth = slots - 1;
    if (depth > 31) depth = 31;
    return depth >= 2 ? depth : 0;
}

int ahci_disk_count(void) {
    int live = 0;
    for (int i = 0; i < g_ndisks; i++) if (g_ports[i].metadata) live++;
    return live;
}

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

static int port_start(struct ahci_port *p)
{
    /* FRE before ST, and only once BSY/DRQ are clear: starting the command
     * engine against a device that still has a transfer in progress is how a
     * port ends up wedged with no error bit to explain it. Not ready is
     * reported, not papered over by setting ST anyway. */
    if (wait_clear(p->reg, P_TFD, TFD_BSY | TFD_DRQ, SPIN_INIT, 1000)) return -1;
    w32(p->reg, P_CMD, r32(p->reg, P_CMD) | CMD_FRE);
    w32(p->reg, P_CMD, r32(p->reg, P_CMD) | CMD_ST);
    return 0;
}

/* Port bring-up when firmware left the link alone (X79-VG4 bring-up,
 * 2026-10-31). Observed under OVMF booting from a USB stick: the HBA's ports
 * report DET=3 but PxSIG still holds its reset value 0xffffffff, because no
 * one programmed a receive area, enabled FIS receive or reset the link, so
 * the drive's signature FIS was never posted (AHCI 1.3.1 sections 3.3.10 and
 * 10.1.2; FRE gates posting the FIS to memory and the TFD update). An AMI
 * board in "fast boot" mode can leave SATA the same way. A DET=1 port may
 * also still show SIG_SATA from before its link dropped, since COMINIT does
 * not clear PxSIG -- so the signature alone never proves a link. */
static int port_needs_reset(uint32_t det, uint32_t sig)
{
#ifndef AHCI_NEGCTL_SIG_ONLY
    return det != 3 || sig != SIG_SATA;
#else
    (void)det;
    return sig != SIG_SATA;
#endif
}

/* Originally described as interrupt-independent: boot uses calibrated TSC
 * with IF=0. Correction: time_mono_ns can fall back to PIT, which needs IF=1;
 * storage waiters must enable IRQs under g_ata_busy for that clock source. */
static uint64_t link_ms(void) { return time_mono_ns() / 1000000ull; }

/* With CLB/FB programmed, the receive area owned by the DMA layer and ST
 * clear: enable FRE, COMRESET (SCTL.DET=1, read back, hold >= 1 ms, then 0,
 * keeping the SPD/IPM limits, as Linux sata_link_hardreset() does), wait for
 * DET=3, acknowledge the DIAG bits the reset sets in SERR, then wait for the
 * signature FIS with BSY and DRQ clear. 0 = ready with *sig valid;
 * -1 = no link within 1 s; -2 = the device never became ready (10 s). Every
 * wait is bounded by a spin count as well as by the clock. */
static int port_link_reset(volatile uint8_t *pr, uint32_t *sig)
{
    w32(pr, P_CMD, r32(pr, P_CMD) | CMD_FRE);
    uint32_t sctl = r32(pr, P_SCTL) & ~0xFu;
#ifndef AHCI_NEGCTL_NO_COMRESET
    w32(pr, P_SCTL, sctl | 1);
    (void)r32(pr, P_SCTL);                               /* post the write before timing it */
    uint64_t hold = link_ms() + 2;
    for (long n = 0; n < SPIN_INIT && link_ms() < hold; n++) { }
    w32(pr, P_SCTL, sctl);
#else
    (void)sctl;
#endif
    uint64_t deadline = link_ms() + 1000;
    int link = 0;
    for (long n = 0; n < SPIN_INIT; n++) {
        if ((r32(pr, P_SSTS) & 0xF) == 3) { link = 1; break; }
        if ((n & 0xFFFF) == 0xFFFF && link_ms() > deadline) break;
    }
    w32(pr, P_SERR, r32(pr, P_SERR));
    *sig = r32(pr, P_SIG);
    if (!link) return -1;
    deadline = link_ms() + 10000;
    for (long n = 0; n < SPIN_INIT * 10; n++) {
        uint32_t tfd = r32(pr, P_TFD);
        *sig = r32(pr, P_SIG);
        if (!(tfd & (TFD_BSY | TFD_DRQ)) && *sig != 0xFFFFFFFFu) return 0;
        if ((n & 0xFFFF) == 0xFFFF && link_ms() > deadline) break;
    }
    return -2;
}

/* Clear the accumulated error registers and re-run the engines. Called between
 * command retries -- a task-file error leaves ST set but the port refusing to
 * make progress until SERR is acknowledged. */
/* Acknowledged CR/FR stop is the release boundary, including the final
 * failed retry. Keeping pages pinned alone would not stop caller reuse. */
static void port_quiesce(struct ahci_port *p)
{
    if (port_stop(p)) {
        dma_device_quarantine(&p->dma_dev);
        panic("ahci: DMA stop unconfirmed; caller memory cannot be reused");
    }
    dma_device_quiesced(&p->dma_dev);
}

/* Latch before W1C, as in AHCI 1.3.1 3.3.5 and Linux v6.12 libahci.c
 * ahci_port_intr. Correction to the old atomic-words-only claim: an atomic OR
 * can still resurrect TFES read before recovery cleared the latch. Snapshot
 * the error epoch BEFORE MMIO, then validate and publish under irq_gate.
 * Recovery takes the same gate to discard the old epoch, so it cannot race
 * between our validation and OR/W1C. A stale collector must skip W1C too:
 * acknowledging its old bits could erase a completion from the replay.
 * io_lock_enter masks local IRQs, preventing same-CPU ISR lock inversion;
 * the ISR never takes the medium gate, and this gate covers no DMA or waits.
 * Model evidence only: not verified on X79-VG4 hardware. */
static uint32_t ahci_port_irq_collect(struct ahci_port *p)
{
    uint64_t epoch = __atomic_load_n(&p->error_epoch, __ATOMIC_ACQUIRE);
    uint32_t status = r32(p->reg, P_IS);
    uint64_t flags = io_lock_enter(&p->irq_gate);
#ifndef AHCI_NEGCTL_COLLECT_STALE_EPOCH
    if (epoch != __atomic_load_n(&p->error_epoch, __ATOMIC_ACQUIRE)) {
        io_lock_leave(&p->irq_gate, flags);
        return 0;
    }
#else
    (void)epoch;
#endif
    __atomic_fetch_or(&p->is_latched, status, __ATOMIC_ACQ_REL);
    if (status) w32(p->reg, P_IS, status);
    io_lock_leave(&p->irq_gate, flags);
    return status;
}

/* Retire a stopped queue's interrupt history before issuing READ LOG or
 * replaying data. The medium gate already owns recovery; irq_gate excludes
 * only collectors, including one that read PxIS on another CPU before stop.
 * Both the epoch change and latch reset must stay inside this short section.
 * PxIS is W1C (AHCI 1.3.1 3.3.5); no new command is issued while it is cleared.
 * Model evidence only: not verified on X79-VG4 hardware. */
static void ahci_clear_errors(struct ahci_port *p)
{
    uint64_t flags = io_lock_enter(&p->irq_gate);
    w32(p->reg, P_IS, r32(p->reg, P_IS));
    __atomic_store_n(&p->is_latched, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&p->error_epoch, 1, __ATOMIC_RELEASE);
    io_lock_leave(&p->irq_gate, flags);
}

/* Each HBA has a single MSI. Clear port sources BEFORE the host level latch,
 * including implemented ports without disks, or an orphan status can storm.
 * Previously: "No gate ... is permitted here." Correction: the collector now
 * takes only the short irq_gate to synchronize its epoch with recovery. It
 * never takes the medium gate or BKL, allocates, transitions DMA or uses blk_req.
 * Source: Linux v6.12 libahci.c ahci_single_level_irq_intr.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/ata/libahci.c */
static void ahci_isr(void *arg)
{
    struct ahci_ctl *ctl = arg;
    int handled = 0;
    for (unsigned pass = 0; pass < 4; pass++) {
        uint32_t pending = r32(ctl->reg, HBA_IS);
        if (!pending) break;
        handled = 1;
#ifdef AHCI_NEGCTL_ISR_CLEAR_ORDER
        w32(ctl->reg, HBA_IS, pending);
#endif
        for (unsigned index = 0; index < 32; index++) {
            if (!(pending & (1u << index))) continue;
            struct ahci_port *port = NULL;
            for (int disk = 0; disk < g_ndisks; disk++) {
                if (g_ports[disk].ctl == ctl && g_ports[disk].index == (int)index) {
                    port = &g_ports[disk];
                    break;
                }
            }
            if (port) {
                (void)ahci_port_irq_collect(port);
                /* Completion means both CI and SActive clear (AHCI 1.3.1
                 * 3.3.13 PxSACT, 3.3.14 PxCI; Linux v6.12 libahci.c
                 * ahci_handle_port_interrupt). Do not read request/mapping
                 * state in the ISR. Serialize this register observation with
                 * issue's bitmap reset and doorbell, so an old idle-slot
                 * observation cannot credit a newly issued command. */
                uint64_t flags = io_lock_enter(&port->irq_gate);
                uint32_t running = r32(port->reg, P_SACT) | r32(port->reg, P_CI);
                __atomic_fetch_or(&port->irq_completed, ~running, __ATOMIC_RELEASE);
                io_lock_leave(&port->irq_gate, flags);
#ifdef AHCI_NEGCTL_ISR_DROP_TFES
                __atomic_fetch_and(&port->is_latched, ~IS_TFES, __ATOMIC_ACQ_REL);
#endif
                __atomic_fetch_add(&port->irq_events, 1, __ATOMIC_RELEASE);
            } else if (ctl->pi & (1u << index)) {
                volatile uint8_t *reg = ctl->reg + 0x100 + index * 0x80;
                w32(reg, P_IS, r32(reg, P_IS));
            }
        }
#ifndef AHCI_NEGCTL_ISR_CLEAR_ORDER
        w32(ctl->reg, HBA_IS, pending);
#endif
    }
    if (!handled) {
        for (int disk = 0; disk < g_ndisks; disk++) {
            if (g_ports[disk].ctl == ctl)
                __atomic_fetch_add(&g_ports[disk].st.spurious, 1, __ATOMIC_RELAXED);
        }
    }
}

/* Completed-but-unreaped slots stay reserved by req. Hardware may finish early;
 * reusing such a slot would let the later poll consume another request's DMA. */
static int ahci_slot_alloc(struct ahci_port *p, int ncq)
{
    if (!ncq && p->active_mask) return -1;
    if (ncq && (p->active_mask & ~p->ncq_mask)) return -1;
    unsigned limit = ncq ? p->qdepth : 1;
    for (unsigned slot = 0; slot < limit; slot++) {
        if (!p->slot[slot].req && p->slot[slot].state != AHCI_SLOT_ACTIVE)
            return (int)slot;
    }
    return -1;
}

/* Release a streaming map only after completion or acknowledged engine stop.
 * The slot retains its request until blk_poll consumes the terminal state. */
static void ahci_unmap(struct ahci_port *p, unsigned slot)
{
    struct ahci_slot *command = &p->slot[slot];
    if (command->mapping) {
        if (dma_unmap(command->mapping)) panic("ahci: unmap before DMA stopped");
        command->mapping = NULL;
        command->map_token = 0;
    }
}

/* Build the slot's command list entry, CFIS and PRDT, then publish SACT before
 * CI. PxSACT/PxCI are write-one-to-set: write ONLY the new bit, rather than a
 * read/OR/write that could resurrect a tag hardware cleared between accesses.
 * Source: AHCI 1.3.1 3.3.13/3.3.14, 4.2; Linux v6.12 libahci.c ahci_qc_issue,
 * ahci_qc_prep and libata-core.c ata_build_rw_tf (FPDMA count in Features). */
static int ahci_issue(struct ahci_port *p, unsigned slot)
{
    struct ahci_slot *command = &p->slot[slot];
    const struct ahci_cmdspec *s = &command->cur;
    uint32_t *hdr = (uint32_t *)(p->dma + slot * 32);
    uint8_t *tbl = p->dma + AHCI_CT_OFF(slot);
    uint8_t *fis = tbl;
    uint8_t *prd = tbl + 0x80;

    /* With a live NCQ queue H:CheckBusy belongs to the HBA, not the CPU.
     * Waiting on TFD here would serialize a queue that is already running. */
    if (!p->active_mask &&
        wait_clear(p->reg, P_TFD, TFD_BSY | TFD_DRQ, SPIN_INIT, 2000)) return -1;
    memset(tbl, 0, AHCI_CT_STRIDE);
    unsigned nprd = 0;
    if (s->bytes) {
        if (!command->mapping || command->mapping->nsegments > AHCI_PRDT) return -1;
        for (size_t i = 0; i < command->mapping->nsegments; i++) {
            const struct dma_segment *segment = &command->mapping->segments[i];
            uint64_t addr = dma_addr_value(segment->addr);
            if (!segment->len || segment->len > AHCI_PRD_MAX) return -1;
            uint32_t *entry = (uint32_t *)(prd + nprd * 16);
            entry[0] = (uint32_t)addr;
            entry[1] = (uint32_t)(addr >> 32);
            entry[2] = 0;
            entry[3] = (uint32_t)segment->len - 1;
            nprd++;
        }
    }
    /* Register H2D FIS (SATA 3.x 10.3.4); command bit, LBA mode, no FUA.
     * ACS READ/WRITE FPDMA QUEUED: Count[7:3]=tag, Features=sector count.
     * PRIO and ICC remain zero; QEMU and real drives may reject other values. */
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = s->command;
    fis[4] = (uint8_t)s->lba;
    fis[5] = (uint8_t)(s->lba >> 8);
    fis[6] = (uint8_t)(s->lba >> 16);
    fis[7] = 0x40;
    fis[8] = (uint8_t)(s->lba >> 24);
    fis[9] = (uint8_t)(s->lba >> 32);
    fis[10] = (uint8_t)(s->lba >> 40);
    if (s->ncq) {
        fis[3] = (uint8_t)s->sectors;
        fis[11] = (uint8_t)(s->sectors >> 8);
#ifndef AHCI_NEGCTL_NCQ_TAG_ZERO
        fis[12] = (uint8_t)(slot << 3);
#endif
    } else {
        fis[12] = (uint8_t)s->sectors;
        fis[13] = (uint8_t)(s->sectors >> 8);
        /* Legacy READ/WRITE DMA still needs the upper LBA28 nibble. */
        if (s->command == ATA_READ_DMA || s->command == ATA_WRITE_DMA)
            fis[7] |= (uint8_t)((s->lba >> 24) & 15);
    }
    hdr[0] = 5u | (s->write ? 1u << 6 : 0u) | (nprd << 16);
    hdr[1] = 0;
    uint64_t table_dma = dma_addr_value(dma_addr_add(p->metadata->dma, AHCI_CT_OFF(slot)));
    hdr[2] = (uint32_t)table_dma;
    hdr[3] = (uint32_t)(table_dma >> 32);
    if (command->mapping) {
        command->map_token = dma_mapping_submit(command->mapping);
        if (!command->map_token) return -1;
    }
    /* Clear stale success status only for a new empty queue. Errors stay
     * latched until recovery; issuing a sibling must never discard TFES. */
    (void)ahci_port_irq_collect(p);
    if (!p->active_mask)
        __atomic_fetch_and(&p->is_latched, IS_TFES | AHCI_IS_FATAL, __ATOMIC_ACQ_REL);
    uint32_t bit = 1u << slot;
    p->active_mask |= bit;
    if (s->ncq) p->ncq_mask |= bit;
    command->state = AHCI_SLOT_ACTIVE;
    command->irq_start = __atomic_load_n(&p->irq_events, __ATOMIC_ACQUIRE);
    /* Retain the 8000 ms budget using the monotonic clock. Correction: ktime
     * may fall back to PIT, so this is not always interrupt-independent.
     * Block admission enables IF under g_ata_busy while draining its gate. */
    command->deadline = link_ms() + 8000;
    /* PxTFD is the last received taskfile, not this command's status until a
     * new FIS arrives (AHCI 1.3.1 3.3.8). ST=0 does not clear its old ERR.
     * Remember that old bit so a replay is not aborted again before the HBA
     * has even sent its command. New TFES remains authoritative throughout. */
    command->stale_error = !!(r32(p->reg, P_TFD) & TFD_ERR);
    barrier();
    /* Pair the new command with a fresh ISR-completion observation. This short
     * gate excludes the ISR's register snapshot through SACT -> CI publication;
     * no DMA operation or wait belongs inside it. See ahci_isr above. */
    uint64_t flags = io_lock_enter(&p->irq_gate);
    __atomic_fetch_and(&p->irq_completed, ~bit, __ATOMIC_ACQ_REL);
#ifdef AHCI_NEGCTL_RECOVER_REPLAY_CI_FIRST
    if (p->recovering && s->ncq) w32(p->reg, P_CI, bit);
#endif
#ifndef AHCI_NEGCTL_NCQ_NO_SACT
    if (s->ncq) w32(p->reg, P_SACT, bit);
#endif
    w32(p->reg, P_CI, bit);
    io_lock_leave(&p->irq_gate, flags);
    unsigned count = (unsigned)__builtin_popcount(p->active_mask);
    if (count > p->st.max_inflight) p->st.max_inflight = count;
    return 0;
}

/* One observation, shared by interrupt-assisted and pure polling operation.
 * NCQ completion is SActive, never PRDBC: Linux v6.12 libahci.c
 * ahci_handle_port_interrupt uses the same active-register distinction. */
static int ahci_check(struct ahci_port *p, unsigned slot)
{
    (void)ahci_port_irq_collect(p);
    uint32_t status = __atomic_load_n(&p->is_latched, __ATOMIC_ACQUIRE);
    if (status & (IS_TFES | AHCI_IS_FATAL))
        return -1;
    struct ahci_slot *command = &p->slot[slot];
    unsigned reg = command->cur.ncq ? P_SACT : P_CI;
    /* A new naked taskfile ERR is also a recovery trigger. Correction to the
     * old READ-LOG-only exception: reissued nonqueued commands can also still
     * see the preceding ERR until their first response FIS (3.3.8/6.2.2.1).
     * A completed command with ERR always fails, and fresh TFES was checked
     * above; only the unchanged old bit on a running command is ignored. */
    uint32_t running = r32(p->reg, reg) & (1u << slot);
    uint32_t taskfile = r32(p->reg, P_TFD);
    if (!(taskfile & TFD_ERR)) command->stale_error = 0;
    int old_error = command->stale_error;
#ifdef AHCI_NEGCTL_RETRY_STALE_ERR
    old_error = p->recovering;
#endif
    if ((taskfile & TFD_ERR) && (!old_error || !running)) return -1;
    if (running) return 0;
    dma_rmb();
    const uint32_t *header = (const uint32_t *)(p->dma + slot * 32);
    if (!command->cur.ncq && !command->cur.write && command->cur.bytes &&
        header[1] < command->cur.bytes) return -1;
    return 1;
}

/* Start one independently mapped command; no DMA ownership exists on refusal.
 * Internal commands also use this path, but their reserved slot never holds a
 * blk_req. Nonqueued commands are fences, including IDENTIFY and READ LOG. */
static int ahci_begin(struct ahci_port *p, unsigned slot, const struct ahci_cmdspec *s)
{
    if (slot >= p->nslots || p->dma_dev.blocked) return -1;
#ifndef AHCI_NEGCTL_MIX_NONQUEUED
    if ((!s->ncq && p->active_mask) ||
        (s->ncq && (p->active_mask & ~p->ncq_mask))) return BLK_E_AGAIN;
#endif
    struct ahci_slot *command = &p->slot[slot];
    if (command->mapping || command->state == AHCI_SLOT_ACTIVE) return BLK_E_AGAIN;
    command->cur = *s;
    command->attempt = 0;
    command->timeouts = 0;
    if (s->bytes) {
        command->mapping = dma_map_kernel(&p->dma_dev, s->buf, s->bytes,
                                          s->write ? DMA_TO_DEVICE : DMA_FROM_DEVICE, 0);
        if (!command->mapping) return -1;
    }
    int rc = ahci_issue(p, slot);
    if (rc) ahci_unmap(p, slot);
    return rc;
}

/* Publish successful DMA before any device-wide quiesce invalidates its token.
 * A refused completion remains a failure; fabricated success loses read data. */
static int ahci_complete_slot(struct ahci_port *p, unsigned slot)
{
    struct ahci_slot *command = &p->slot[slot];
    int status = 0;
    if (command->mapping &&
        dma_mapping_complete(command->mapping, command->map_token, command->cur.bytes))
        status = -1;
    ahci_unmap(p, slot);
    command->state = status ? AHCI_SLOT_FAILED : AHCI_SLOT_DONE;
    if (command->req) command->req->status = status;
    p->active_mask &= ~(1u << slot);
    p->ncq_mask &= ~(1u << slot);
    return status;
}

/* Exhausted retry or restart failure is terminal for the whole port. Confirm
 * stop BEFORE releasing any caller mapping; blocked remains set permanently.
 * Existing callers own request storage until their normal blk_poll consumes it. */
static int ahci_fail_port(struct ahci_port *p)
{
    port_quiesce(p);
    for (unsigned slot = 0; slot < p->nslots; slot++) {
        struct ahci_slot *command = &p->slot[slot];
        if (command->state != AHCI_SLOT_ACTIVE) continue;
        ahci_unmap(p, slot);
        command->state = AHCI_SLOT_FAILED;
        if (command->req) command->req->status = -1;
    }
    p->active_mask = 0;
    p->ncq_mask = 0;
    return -1;
}

/* A link reset is counted separately from engine restart. FRE must be running
 * for the signature FIS; port_link_reset preserves the old SCTL speed limits.
 * Source: Linux v6.12 libahci.c ahci_hardreset and AHCI 1.3.1 10.1.2. */
static int ahci_reset_link(struct ahci_port *p, const char *reason)
{
    uint32_t signature;
    p->st.resets++;
    kprintf("[ahci] %s: COMRESET (%s)\n", p->name, reason);
    /* READ LOG failure may already have quiesced the entire DMA context.
     * COMRESET needs FRE for its signature FIS, so renew METADATA ownership
     * before enabling that receiver. Streaming maps stay quiesced, ST stays 0.
     * Afterwards acknowledge FR=0 again before invalidating metadata's token:
     * the initial engine stop does not cover this newly enabled FIS receiver.
     * AHCI 1.3.1 3.3.7 / 10.1.2; same FRE-before-signature rule as bring-up. */
    if (p->dma_dev.blocked) {
        if (dma_device_resume(&p->dma_dev) || !dma_buffer_submit(p->metadata)) return -1;
    }
    int result = port_link_reset(p->reg, &signature);
    port_quiesce(p);
    return result || signature != SIG_SATA ? -1 : 0;
}

/* Read ACS NCQ Command Error log (address 10h, one 512-byte sector) in the
 * reserved slot. No recursive recovery here: failure goes back to the owner,
 * which COMRESETs before replaying. Source: Linux v6.12 libata-sata.c
 * ata_eh_read_log_10h / ata_eh_analyze_ncq_error (moved there from libata-eh.c
 * in 2020); byte0 NQ means no valid tag.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/ata/libata-sata.c */
static int ahci_read_log10(struct ahci_port *p, struct ahci_ncq_err *out)
{
    /* 512-byte alignment keeps this single-sector log within one CPU page,
     * so a scattered kernel stack cannot require a second PRD for the log. */
    uint8_t log[512] __attribute__((aligned(512)));
    unsigned slot = p->nslots - 1;
    struct ahci_cmdspec spec = {0x2f, 0, 0x10, 1, log, sizeof log, 0, 0};
    if (ahci_begin(p, slot, &spec)) return -1;
    /* ahci_begin/ahci_issue already assigned this slot the normal 8000 ms
     * command budget. The former separate 1000 ms limit had no device/spec
     * basis: a valid slow log would cause an unnecessary COMRESET and lose
     * selective retry attribution. Keep the same deadline and SPIN_CMD bound
     * as other commands, without extending it on each observation.
     * Policy tested in the host model, not verified on X79-VG4 hardware. */
    uint64_t deadline = p->slot[slot].deadline;
#ifdef AHCI_NEGCTL_LOG_SHORT_BUDGET
    deadline = link_ms() + 1000;
#endif
    int result = 0;
    for (long spin = 0; spin < SPIN_CMD; spin++) {
        result = ahci_check(p, slot);
        if (result || link_ms() >= deadline) break;
        AHCI_PAUSE();
    }
    if (result != 1) {
        port_quiesce(p);
        ahci_unmap(p, slot);
        p->slot[slot].state = AHCI_SLOT_FREE;
        p->active_mask &= ~(1u << slot);
        return -1;
    }
    if (ahci_complete_slot(p, slot)) return -1;
    p->slot[slot].state = AHCI_SLOT_FREE;
    if (log[0] & 0x80) return -1;
    out->tag = log[0] & 31;
    out->status = log[2];
    out->error = log[3];
    out->lba = (uint64_t)log[4] | (uint64_t)log[5] << 8 | (uint64_t)log[6] << 16 |
               (uint64_t)log[8] << 24 | (uint64_t)log[9] << 32 | (uint64_t)log[10] << 40;
    return 0;
}

/* Recover the entire port, attributing retry cost to the failing NCQ tag only.
 * Completed maps MUST be consumed before dma_device_quiesced: it invalidates
 * ALL tokens, not just the tag being polled. Source: AHCI 1.3.1 6.2.2;
 * Linux v6.12 libahci.c ahci_kick_engine and libata-sata.c ata_eh_analyze_ncq_error.
 * Model/QEMU evidence only, not verified on X79-VG4 hardware. */
static int ahci_recover(struct ahci_port *p, int cause)
{
    if (p->recovering) return ahci_fail_port(p);
    /* The old READ LOG wait inherited IF=0 from ordinary blk_poll's medium
     * gate. Its 8000 ms deadline then froze on the PIT fallback, leaving only
     * SPIN_CMD as a limit. Cover the WHOLE recovery (including COMRESET's
     * minimum hold time) with the same busy-before-STI discipline as
     * blkdev.c wait_locked and ahci_run. The AHCI ISR never takes that gate;
     * g_ata_busy prevents scheduling while its recovery owner enables IRQs.
     * Restore IF before dropping busy, even on fail-stop cleanup returns.
     * IF is RFLAGS bit 9 (Intel SDM Vol. 3A, 6.3.2); clock fallback is in
     * c/kernel/core/ktime.c time_mono_ns. Model evidence only:
     * not verified on X79-VG4 hardware. */
    uint64_t flags;
    AHCI_FLAGS(flags);
    __atomic_fetch_add(&g_ata_busy, 1, __ATOMIC_RELAXED);
#ifndef AHCI_NEGCTL_RECOVERY_IF0
    AHCI_STI();
#endif
    int result;
    p->recovering = 1;
    p->st.errors++;
    p->st.recoveries++;
    uint32_t queued = p->ncq_mask;
    uint32_t active = p->active_mask;
    /* Correction to the old "some HBAs clear CI" claim: AHCI 1.3.1 6.2.2.2
     * requires stop to reset BOTH CI and SACT. Snapshot before clearing ST;
     * after CR/FR acknowledge, DMA cannot race the successful unmaps.
     * A clear CI alone does not validate nonqueued data: short PRDBC or ERR
     * must remain a retry, never publish bytes the device did not transfer.
     * https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf */
    uint32_t running_ncq = r32(p->reg, P_SACT);
    uint32_t running_dma = r32(p->reg, P_CI);
    uint32_t taskfile = r32(p->reg, P_TFD);
    uint32_t interrupts = __atomic_load_n(&p->is_latched, __ATOMIC_ACQUIRE);
#ifndef AHCI_NEGCTL_RECOVER_NO_STOP
    if (port_stop(p)) {
        dma_device_quarantine(&p->dma_dev);
        panic("ahci: DMA stop unconfirmed; caller memory cannot be reused");
    }
#endif
#ifdef AHCI_NEGCTL_RECOVER_POSTSTOP_SNAPSHOT
    running_ncq = r32(p->reg, P_SACT);
    running_dma = r32(p->reg, P_CI);
#endif
#ifdef AHCI_NEGCTL_COMPLETE_AFTER_QUIESCE
    dma_device_quiesced(&p->dma_dev);
#endif
    for (unsigned slot = 0; slot < p->nslots; slot++) {
        uint32_t bit = 1u << slot;
        if (!(active & bit)) continue;
        struct ahci_slot *command = &p->slot[slot];
        uint32_t running = command->cur.ncq ? running_ncq : running_dma;
        if (running & bit) continue;
#ifndef AHCI_NEGCTL_RECOVER_SHORT_SUCCESS
        if (!command->cur.ncq) {
            const uint32_t *header = (const uint32_t *)(p->dma + slot * 32);
            dma_rmb();
            if ((taskfile & TFD_ERR) || (interrupts & (IS_TFES | AHCI_IS_FATAL)) ||
                (!command->cur.write && header[1] < command->cur.bytes)) continue;
        }
#endif
        (void)ahci_complete_slot(p, slot);
    }
    active = p->active_mask;
    uint32_t serr = r32(p->reg, P_SERR);
    uint32_t tfd = r32(p->reg, P_TFD);
#ifdef AHCI_NEGCTL_RECOVER_CLEAR_ORDER
    w32(p->reg, P_IS, r32(p->reg, P_IS));
#endif
    w32(p->reg, P_SERR, UINT32_MAX);
    ahci_clear_errors(p);
    int reset = 0;
    if (tfd & (TFD_BSY | TFD_DRQ)) {
        if (p->cap & CAP_SCLO) {
            /* CLO is legal only with ST=0; wait for hardware self-clear
             * (AHCI 1.3.1 3.3.7; Linux v6.12 libahci.c ahci_kick_engine).
             * Correction to the planned "CLO replaces COMRESET" rule:
             * clearing the HBA's BSY/DRQ shadow does NOT stop the device's
             * command. Even successful CLO must be followed by device reset
             * before replay. We use COMRESET, not an unimplemented SRST FIS
             * sequence (AHCI 1.3.1 6.2.2.1/6.2.2.2 and 10.4.1).
             * https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf
             * Model evidence only, not verified on X79-VG4 hardware. */
            w32(p->reg, P_CMD, r32(p->reg, P_CMD) | CMD_CLO);
            if (wait_clear(p->reg, P_CMD, CMD_CLO, SPIN_INIT, 500)) reset = 1;
        } else reset = 1;
#ifndef AHCI_NEGCTL_CLO_NO_RESET
        reset = 1;
#endif
    }
    if (cause == AHCI_RC_TIMEOUT) {
        /* Correction to the planned "first timeout only stop/start" rule:
         * ST=0 clears HBA CI/SACT, but a disk with BSY=0 may still own every
         * NCQ tag. CLO only clears the HBA taskfile busy handshake. Reusing
         * those tags without cancelling the DEVICE queue can duplicate I/O.
         * Reset on the FIRST NCQ timeout, before any replay; only a reported
         * NCQ error can instead use the READ LOG 10h cancellation path below.
         * Source: libATA Developer's Guide, "Unknown error (timeout)":
         * https://www.kernel.org/doc/html/latest/driver-api/libata.html#unknown-error-timeout
         * Model evidence only, not verified on X79-VG4 hardware. */
#ifndef AHCI_NEGCTL_NCQ_TIMEOUT_NO_RESET
        if (queued & active) reset = 1;
#endif
        for (unsigned slot = 0; slot < p->nslots; slot++) {
            if ((active & (1u << slot)) && p->slot[slot].timeouts >= 2) reset = 1;
        }
    }
    if (cause == AHCI_RC_FATAL) reset = 1;
    if (reset && ahci_reset_link(p, cause == AHCI_RC_TIMEOUT ?
                                "command timeout" : "busy/fatal error")) goto failed;
    dma_device_quiesced(&p->dma_dev);
    if (dma_device_resume(&p->dma_dev) || !dma_buffer_submit(p->metadata)) goto failed;
#ifdef AHCI_NEGCTL_RECOVER_LOG_BEFORE_START
    if (queued && cause == AHCI_RC_TFES && !reset) {
        struct ahci_ncq_err error;
        p->active_mask = 0;
        p->ncq_mask = 0;
        (void)ahci_read_log10(p, &error);
    }
#endif
    if (port_start(p)) goto failed;

    /* Suspend software masks while the reserved nonqueued log command runs.
     * Data slots and their mappings remain private to this recovery owner. */
    p->active_mask = 0;
    p->ncq_mask = 0;
    int tag = -1;
    if (queued && cause == AHCI_RC_TFES && !reset) {
#ifndef AHCI_NEGCTL_NO_LOG10
        struct ahci_ncq_err error;
        if (!ahci_read_log10(p, &error) && (active & (1u << error.tag))) {
            tag = error.tag;
            kprintf("[ahci] %s: ncq error tag=%u status=%02x error=%02x lba=%llu "
                    "serr=%x tfd=%x -> retry %u/8\n",
                    p->name, error.tag, error.status, error.error,
                    (unsigned long long)error.lba, serr, tfd, p->slot[tag].attempt + 1);
        } else {
            /* Failed log DMA must be stopped before COMRESET and metadata
             * ownership renewed again. NQ/invalid tags never guess a victim. */
            port_quiesce(p);
            if (ahci_reset_link(p, "NCQ log unavailable")) goto failed;
            if (dma_device_resume(&p->dma_dev) || !dma_buffer_submit(p->metadata) ||
                port_start(p)) goto failed;
            ahci_clear_errors(p);
        }
#endif
    }
    (void)serr; /* NO_LOG10 control omits the diagnostic that consumes it. */
    if (queued && ++p->ncq_recoveries >= 3 && p->qdepth > 1) {
        p->qdepth = 1;
        blk_set_queue_depth(p->block, 1);
        kprintf("[ahci] %s: ncq disabled after repeated errors\n", p->name);
    }
    for (unsigned slot = 0; slot < p->nslots; slot++) {
        if (!(active & (1u << slot))) continue;
        if (tag < 0 || tag == (int)slot) p->slot[slot].attempt++;
        if (p->slot[slot].attempt >= AHCI_RETRIES) goto failed;
    }
    for (unsigned slot = 0; slot < p->nslots; slot++) {
        if (!(active & (1u << slot))) continue;
        if (ahci_issue(p, slot)) goto failed;
    }
    p->recovering = 0;
    result = 0;
    goto out;
failed:
    p->recovering = 0;
    result = ahci_fail_port(p);
out:
    if (!(flags & 0x200)) AHCI_CLI();
    __atomic_fetch_sub(&g_ata_busy, 1, __ATOMIC_RELAXED);
    return result;
}

/* Drive one slot once. A recovery may finish a sibling, but the block layer
 * alone publishes BLK_REQ_DONE and removes its own list entry under the gate. */
static int ahci_step(struct ahci_port *p, unsigned slot, int expired)
{
    struct ahci_slot *command = &p->slot[slot];
    if (command->state == AHCI_SLOT_DONE) return 1;
    if (command->state == AHCI_SLOT_FAILED) return -1;
    int result = ahci_check(p, slot);
    if (result == 1) {
        uint32_t events = __atomic_load_n(&p->irq_events, __ATOMIC_ACQUIRE);
        /* Previously compared events to irq_start for the whole port. A
         * sibling's early IRQ then hid this slot's later polling completion.
         * Count each command by whether the ISR observed THIS slot finished;
         * issue resets its bit when the slot is reused, including chunking.
         * Model evidence only: not verified on X79-VG4 hardware. */
#ifdef AHCI_NEGCTL_POLL_PORT_GENERATION
        if (events == command->irq_start) p->st.poll_done++;
#else
        uint32_t completed = __atomic_load_n(&p->irq_completed, __ATOMIC_ACQUIRE);
        if (!(completed & (1u << slot))) p->st.poll_done++;
#endif
        /* Only the MSI destination owns the idle generation. An AP completing
         * a sibling must not consume the BSP's wake-up between poll and HLT;
         * that would turn a delivered MSI into a whole timer-tick delay. */
        if (p->ctl && p->ctl->irq_armed && lapic_id() == p->ctl->irq_cpu)
            p->irq_seen = events;
        return ahci_complete_slot(p, slot) ? -1 : 1;
    }
    if (!result && !expired) return 0;
    int cause = AHCI_RC_TFES;
    if (!result) {
        command->timeouts++;
        cause = AHCI_RC_TIMEOUT;
    } else if (__atomic_load_n(&p->is_latched, __ATOMIC_ACQUIRE) & AHCI_IS_FATAL) {
        cause = AHCI_RC_FATAL;
    }
    if (ahci_recover(p, cause)) return -1;
    if (command->state == AHCI_SLOT_DONE) return 1;
    if (command->state == AHCI_SLOT_FAILED) return -1;
    return 0;
}

/* Internal IDENTIFY uses the reserved slot and the same command/DMA state
 * machine as block I/O. Temporarily enable IF even during kmain, preserving
 * the busy-before-sti order that prevents preemption during a storage wait. */
static int ahci_run(struct ahci_port *p, const struct ahci_cmdspec *s)
{
    uint64_t flags;
    AHCI_FLAGS(flags);
    __atomic_fetch_add(&g_ata_busy, 1, __ATOMIC_RELAXED);
    AHCI_STI();
    unsigned slot = p->nslots - 1;
    int rc = -1;
    if (!ahci_begin(p, slot, s)) {
        long spins = 0;
        unsigned attempt = p->slot[slot].attempt;
        for (;;) {
            int result = ahci_step(p, slot, spins >= SPIN_CMD);
            if (result) {
                rc = result == 1 ? 0 : -1;
                break;
            }
            if (p->slot[slot].attempt != attempt) {
                attempt = p->slot[slot].attempt;
                spins = 0;
            } else spins++;
            AHCI_PAUSE();
        }
    }
    if (!(flags & 0x200)) AHCI_CLI();
    __atomic_fetch_sub(&g_ata_busy, 1, __ATOMIC_RELAXED);
    return rc;
}

/* HLT is useful only on the MSI destination CPU. An AP otherwise waits for its
 * 1.6--5.4 ms local tick while g_ata_busy prevents scheduling: much slower than
 * PAUSE for microsecond I/O. Check events with IF clear; STI/HLT are adjacent.
 * This is interrupt-assisted waiting, not a scheduler handoff. */
static void ahci_blk_idle(void *ctx)
{
    struct ahci_port *p = ctx;
    int destination = p->ctl && p->ctl->irq_armed && lapic_id() == p->ctl->irq_cpu;
#ifdef AHCI_NEGCTL_IDLE_ANY_CPU
    destination = 1;
#endif
    if (!p->ctl || !p->ctl->irq_armed || !destination) {
        AHCI_PAUSE();
        return;
    }
    AHCI_CLI();
    uint32_t events = __atomic_load_n(&p->irq_events, __ATOMIC_ACQUIRE);
    (void)events; /* IDLE_NO_GUARD control deliberately omits this observation. */
#ifndef AHCI_NEGCTL_IDLE_NO_GUARD
    if (events != p->irq_seen) {
        p->irq_seen = events;
        AHCI_STI();
        return;
    }
#endif
    AHCI_HALT();
}

/* Start one bounded chunk while retaining the request's tag across chunks.
 * DMA mappings cap chunks at 1 MiB; do not split further just to fill a queue.
 * ACS FPDMA 60h/61h is selected only for LBA48 data commands. */
static int ahci_begin_chunk(struct ahci_port *p, struct blk_req *r)
{
    struct ahci_cmdspec spec = {0};
    spec.write = r->op == BLK_OP_WRITE;
    if (r->op == BLK_OP_FLUSH) {
        spec.command = p->lba48 ? ATA_FLUSH_EXT : ATA_FLUSH;
    } else {
        uint64_t lba = r->dev_lba + r->done;
        uint32_t count = r->count - r->done;
        if (count > DMA_MAX_MAPPING / 512) count = DMA_MAX_MAPPING / 512;
        if (!p->lba48) {
            if (count > 255) count = 255;
            if (lba + count > (1u << 28)) return -1;
        }
        /* Finish already-owned NCQ requests in their original protocol after
         * reducing admission depth. Waiting here for a sibling's poll would
         * deadlock blk_submit: under throttling it drives ONLY the oldest
         * request, which may be this multi-chunk one. New requests use DMA EXT
         * once the old request list drains; no new NCQ work is admitted. */
        int continuing_ncq = r->done && p->slot[r->tag].cur.ncq;
        spec.ncq = p->lba48 && (p->qdepth >= 2 || continuing_ncq);
        spec.command = spec.ncq ? (spec.write ? 0x61 : 0x60) :
                       p->lba48 ? (spec.write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT) :
                       (spec.write ? ATA_WRITE_DMA : ATA_READ_DMA);
        spec.lba = lba;
        spec.sectors = count;
        spec.bytes = count * 512u;
        spec.buf = (uint8_t *)r->buf + (size_t)r->done * 512;
    }
    spec.tag = (uint8_t)r->tag;
    r->chunk = spec.sectors;
    return ahci_begin(p, r->tag, &spec);
}

/* Admission may refuse temporarily without changing a live queue. Block bounds
 * include partition translation; recheck against the disk's IDENTIFY capacity. */
static int ahci_blk_submit(void *ctx, struct blk_req *r)
{
    struct ahci_port *p = ctx;
    if (p->dma_dev.blocked) return -1;
    if (r->op != BLK_OP_FLUSH &&
        (!r->count || r->dev_lba >= p->nsectors ||
         r->count > p->nsectors - r->dev_lba)) return -1;
    int ncq = r->op != BLK_OP_FLUSH && p->qdepth >= 2 && p->lba48;
    int slot = ahci_slot_alloc(p, ncq);
#ifdef AHCI_NEGCTL_MIX_NONQUEUED
    if (!ncq && p->active_mask) slot = (int)p->nslots - 1;
#endif
    if (slot < 0) return BLK_E_AGAIN;
    r->tag = (uint32_t)slot;
    p->slot[slot].req = r;
    int result = ahci_begin_chunk(p, r);
    if (result) p->slot[slot].req = NULL;
    return result;
}

/* Reap only the caller's slot. Queue recovery can have completed it already;
 * terminal slots remain reserved until this callback consumes that result. */
static int ahci_blk_poll(void *ctx, struct blk_req *r)
{
    struct ahci_port *p = ctx;
    if (r->tag >= p->nslots) {
        r->status = -1;
        return 1;
    }
    struct ahci_slot *command = &p->slot[r->tag];
    int result = ahci_step(p, r->tag, link_ms() > command->deadline);
    if (!result) return 0;
    if (result > 0) {
        r->done += r->chunk;
        if (r->op != BLK_OP_FLUSH && r->done < r->count) {
            if (!ahci_begin_chunk(p, r)) return 0;
            result = -1;
        }
    }
    r->status = result < 0 ? -1 : 0;
    command->req = NULL;
    command->state = AHCI_SLOT_FREE;
    return 1;
}

static const struct blk_ops ahci_ops = {
    .submit = ahci_blk_submit,
    .poll = ahci_blk_poll,
    .idle = ahci_blk_idle,
};

/* --------------------------------------------------------------------------
 * Discovery
 * ------------------------------------------------------------------------ */

/* An AHCI controller, however it was located. */
struct ahci_hba {
    struct device *model;
    uint16_t vendor, device;
    uint8_t  bus, slot, func;
    uint64_t abar;                  /* BAR5, mapped */
    uint16_t old_command;           /* exact pre-probe PCI Command value */
    int      quarantined;           /* ownership failed; never touch again */
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

/* A controller that failed an ownership transition stays out of every later
 * ahci_init() call. Keeping this small table is enough: this driver deliberately
 * examines at most AHCI_MAX_HBA controllers per boot. */
static uint32_t g_ahci_quarantine[AHCI_MAX_HBA];
static unsigned g_ahci_quarantine_count;

static uint32_t ahci_hba_key(const struct ahci_hba *h)
{
    return 1u + ((uint32_t)h->bus << 8) + ((uint32_t)h->slot << 3) + h->func;
}

static int ahci_hba_is_quarantined(const struct ahci_hba *h)
{
    uint32_t key = ahci_hba_key(h);
    for (unsigned i = 0; i < g_ahci_quarantine_count; i++)
        if (g_ahci_quarantine[i] == key) return 1;
    return 0;
}

static void ahci_hba_quarantine(struct ahci_hba *h)
{
    if (ahci_hba_is_quarantined(h)) { h->quarantined = 1; return; }
    if (g_ahci_quarantine_count >= AHCI_MAX_HBA)
        panic("ahci: controller quarantine ledger exhausted");
    g_ahci_quarantine[g_ahci_quarantine_count++] = ahci_hba_key(h);
    h->quarantined = 1;
}

/* PCI Command is split into two ownership transitions. Discovery enables only
 * MMIO decode; it must never introduce Bus Master before BOHC and the firmware
 * command/FIS engines have been stopped. old_command is retained so any refusal
 * before handoff can restore the exact firmware state. */
static uint16_t ahci_pci_command_read(const struct ahci_hba *h)
{
    return pci_cfg_read16(h->bus, h->slot, h->func, PCI_CFG_COMMAND);
}

static void ahci_pci_command_write(const struct ahci_hba *h, uint16_t command)
{
    pci_cfg_write16(h->bus, h->slot, h->func, PCI_CFG_COMMAND, command);
}

static void ahci_pci_restore_initial(struct ahci_hba *h)
{
    ahci_pci_command_write(h, h->old_command);
    uint16_t restored = ahci_pci_command_read(h);
    if (restored != h->old_command) {
        kprintf("[ahci] %02x:%02x.%u PCI Command restore unconfirmed (old=%x now=%x)\n",
                h->bus, h->slot, h->func, h->old_command, restored);
        panic("ahci: PCI Command restore unconfirmed");
    }
}

static int ahci_pci_mem_enable(struct ahci_hba *h)
{
    uint16_t old = ahci_pci_command_read(h);
    if (old == UINT16_MAX) return -1;
    h->old_command = old;

    /* Preserve a firmware-enabled BME, but never introduce it here. Firmware
     * may still own and use the controller until BOHC completes. */
    uint16_t wanted = (uint16_t)(old | PCI_CMD_MEM);
    ahci_pci_command_write(h, wanted);
    uint16_t after = ahci_pci_command_read(h);
    int decode_ok = (after & PCI_CMD_MEM) != 0;
    int master_unchanged = ((after ^ old) & PCI_CMD_MASTER) == 0;
    if (after != UINT16_MAX && decode_ok && master_unchanged) return 0;

    ahci_pci_restore_initial(h);
    kprintf("[ahci] %02x:%02x.%u PCI MEM enable refused (command=%x mem=%s bme-unchanged=%s)\n",
            h->bus, h->slot, h->func, after, decode_ok ? "yes" : "no",
            master_unchanged ? "yes" : "no");
    return -1;
}

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
        out->model = d;
        out->vendor = d->vendor; out->device = d->device;
        out->bus = d->bus; out->slot = d->slot; out->func = d->func;
        out->abar = 0; out->quarantined = 0;
        if (ahci_hba_is_quarantined(out)) {
            out->quarantined = 1;
            return 0;
        }
        if (ahci_pci_mem_enable(out) != 0) {
            ahci_hba_quarantine(out);
            return 0;
        }
        out->abar = dev_bar_map(d, 5);          /* ABAR is BAR5, always */
        if (!out->abar) {
            ahci_pci_restore_initial(out);
            ahci_hba_quarantine(out);
        }
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
            out->model = NULL;
            out->bus = 0; out->slot = slot; out->func = func;
            out->vendor = id & 0xFFFF; out->device = id >> 16;
            out->abar = 0; out->quarantined = 0;
            if (ahci_hba_is_quarantined(out)) {
                out->quarantined = 1;
                return 0;
            }
            if (bar5) {
                if (ahci_pci_mem_enable(out) == 0) {
                    vmm_map_range(bar5, bar5, 0x2000,
                                  VMM_WRITABLE | VMM_NOCACHE);
                    out->abar = bar5;
                } else ahci_hba_quarantine(out);
            }
            return 0;
        }
    }
    return -1;
}

#endif

/* ACS IDENTIFY word 106 bits 15:14 validate the logical-sector fields.
 * 512e media keeps 256 words per logical sector; bit 13 only describes its
 * physical grouping. Native 4Kn sets bit 12 and words 117/118. Earlier code
 * ignored those words and exposed native-sector LBAs through a 512-byte API. */
static uint64_t ahci_identify_sector_bytes(const uint16_t *id)
{
    if ((id[106] & 0xc000) != 0x4000 || !(id[106] & (1u << 12))) return 512;
    return ((uint64_t)id[117] | ((uint64_t)id[118] << 16)) * 2;
}

/* If firmware retains either ownership or BIOS-busy, no port/GHC write is
 * permitted. Previously both wait failures were explicitly discarded and the
 * driver programmed a controller that firmware could still be using.
 * The existing bounded wait is retained; firmware that does not release within
 * it is refused. This does not claim calibrated real-PC handoff timing. */
static int ahci_bios_handoff(volatile uint8_t *abar)
{
    if (!(r32(abar, HBA_CAP2) & 1u)) return 0;
    w32(abar, HBA_BOHC, r32(abar, HBA_BOHC) | BOHC_OOS);
    if (wait_clear(abar, HBA_BOHC, BOHC_BOS | BOHC_BB, SPIN_INIT, 2000)) {
        kprintf("[ahci] BIOS handoff timed out (bohc=%x); no port/engine writes\n", r32(abar, HBA_BOHC));
        return -1;
    }
    return 0;
}

/* BOHC is the boundary at which it is legal to take DMA away from firmware.
 * Preserve firmware's BME before that boundary; immediately afterwards force a
 * known MEM-on/BME-off Command value and confirm it before inspecting any port.
 * If the clear cannot be confirmed, an old command engine may still be able to
 * DMA through its old CLB/FIS addresses, so there is no safe return path. */
static void ahci_pci_master_quiet(struct ahci_hba *h)
{
    uint16_t safe = (uint16_t)((h->old_command | PCI_CMD_MEM) & ~PCI_CMD_MASTER);
    ahci_pci_command_write(h, safe);
    uint16_t after = ahci_pci_command_read(h);
    if (after != safe) {
        kprintf("[ahci] %02x:%02x.%u cannot quarantine BME after handoff "
                "(want=%x got=%x)\n",
                h->bus, h->slot, h->func, safe, after);
        panic("ahci: BME disable unconfirmed after BIOS handoff");
    }
}

/* Enable DMA only after every firmware engine has been observed stopped. A
 * failed enable is restored to the post-handoff safe state, not old_command:
 * old_command may contain firmware's BME and ownership has already moved to us.
 * If even that isolation write cannot be confirmed, stop the machine. */
static int ahci_pci_master_enable(struct ahci_hba *h)
{
    uint16_t safe = (uint16_t)((h->old_command | PCI_CMD_MEM) & ~PCI_CMD_MASTER);
    uint16_t before = ahci_pci_command_read(h);
    if (before != safe) {
        ahci_pci_command_write(h, safe);
        if (ahci_pci_command_read(h) != safe)
            panic("ahci: pre-BME safe Command state unconfirmed");
    }

    uint16_t wanted = (uint16_t)(safe | PCI_CMD_MASTER);
    ahci_pci_command_write(h, wanted);
    uint16_t after = ahci_pci_command_read(h);
    int mem_ok = (after & PCI_CMD_MEM) != 0;
    int master_ok = (after & PCI_CMD_MASTER) != 0;
    if (after != UINT16_MAX && mem_ok && master_ok) return 0;

    ahci_pci_command_write(h, safe);
    uint16_t isolated = ahci_pci_command_read(h);
    if (isolated != safe) {
        kprintf("[ahci] %02x:%02x.%u BME enable failed and isolation is "
                "unconfirmed (want=%x got=%x)\n",
                h->bus, h->slot, h->func, safe, isolated);
        panic("ahci: BME quarantine failed");
    }
    kprintf("[ahci] %02x:%02x.%u BME enable refused (command=%x mem=%s master=%s); "
            "controller quarantined\n",
            h->bus, h->slot, h->func, after, mem_ok ? "yes" : "no",
            master_ok ? "yes" : "no");
    return -1;
}

/* Stop every implemented firmware port while PCI BME is confirmed clear. ST
 * and FRE are writable requests; CR and FR are the hardware acknowledgements.
 * All four must read clear before any later BME write. PI bits outside CAP.NP
 * are malformed and refused without walking beyond the reported register set. */
static int ahci_stop_firmware_ports(volatile uint8_t *abar, uint32_t cap,
                                    uint32_t pi)
{
    unsigned nports = (cap & 0x1Fu) + 1u;
    uint32_t valid = nports == 32 ? UINT32_MAX : ((1u << nports) - 1u);
    if (pi & ~valid) return -1;

    for (unsigned i = 0; i < nports; i++) {
        if (!(pi & (1u << i))) continue;
        volatile uint8_t *pr = abar + 0x100 + i * 0x80;
        uint32_t cmd = r32(pr, P_CMD);
        w32(pr, P_CMD, cmd & ~CMD_ST);
        if (wait_clear(pr, P_CMD, CMD_ST | CMD_CR, SPIN_INIT, 600))
            return -1;
        cmd = r32(pr, P_CMD);
        w32(pr, P_CMD, cmd & ~CMD_FRE);
        if (wait_clear(pr, P_CMD, CMD_FRE | CMD_FR, SPIN_INIT, 600))
            return -1;
        if (r32(pr, P_CMD) & (CMD_ST | CMD_FRE | CMD_CR | CMD_FR))
            return -1;
    }
    return 0;
}

/* Complete the ownership transaction before ahci_bring_up allocates a byte of
 * DMA memory. On a recoverable refusal the controller remains MEM-decodable,
 * BME-clear and marked quarantined; no caller may touch ABAR again. */
static int ahci_prepare_controller(struct ahci_hba *h, volatile uint8_t *abar,
                                   uint32_t *cap_out, uint32_t *pi_out,
                                   uint32_t *vs_out)
{
    if (ahci_bios_handoff(abar)) {
        ahci_pci_restore_initial(h);
        ahci_hba_quarantine(h);
        return -1;
    }

    ahci_pci_master_quiet(h);

    uint32_t ghc = r32(abar, HBA_GHC);
    w32(abar, HBA_GHC, (ghc | GHC_AE) & ~GHC_IE);
    ghc = r32(abar, HBA_GHC);
    if (!(ghc & GHC_AE) || (ghc & GHC_IE)) {
        ahci_hba_quarantine(h);
        return -1;
    }

    uint32_t cap = r32(abar, HBA_CAP);
    uint32_t pi = r32(abar, HBA_PI);
    uint32_t vs = r32(abar, HBA_VS);
    if (ahci_stop_firmware_ports(abar, cap, pi)) {
        kprintf("[ahci] %02x:%02x.%u firmware engines did not stop; "
                "BME remains off and controller is quarantined\n",
                h->bus, h->slot, h->func);
        ahci_hba_quarantine(h);
        return -1;
    }
    if (ahci_pci_master_enable(h)) {
        ahci_hba_quarantine(h);
        return -1;
    }

    *cap_out = cap; *pi_out = pi; *vs_out = vs;
    return 0;
}

/* IDENTIFY DEVICE: capacity, LBA48 support and the model string, read from the
 * drive rather than assumed. The capacity is what bounds every later request,
 * so getting it from the device is what stops a partition table that claims
 * more sectors than exist from being believed. */
static int port_identify(struct ahci_port *p)
{
    static uint16_t id[256] __attribute__((aligned(64)));   /* identity mapped DMA target */
    memset(id, 0, sizeof id);
    struct ahci_cmdspec s = { ATA_IDENTIFY, 0, 0, 0, id, sizeof id, 0, 0 };
    if (ahci_run(p, &s) != 0) return -1;
    uint64_t sector_bytes = ahci_identify_sector_bytes(id);
    if (sector_bytes != 512) {
        kprintf("[ahci] port %d: logical-sector=%llu unsupported (need 512)\n",
                p->index, (unsigned long long)sector_bytes);
        return -1;
    }

    p->lba48 = (id[83] & (1u << 10)) ? 1 : 0;
    p->ncq_words = id[75] | (uint32_t)id[76] << 16;
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

/* No model-specific exceptions are justified yet. Linux v6.12 libata-core.c
 * ata_device_blacklist uses ATA_HORKAGE_NONCQ for measured drive defects.
 * Add a prefix only with a cited failure; never guess from the Netac brand.
 * Model/QEMU evidence only: not verified on X79-VG4 hardware. */
static const char *const ahci_ncq_horkage[] = { NULL };

/* Keep IDENTIFY policy at the driver boundary. LBA28 cannot use FPDMA, and a
 * known broken drive must remain usable through the existing DMA commands. */
static void ahci_configure_queue(struct ahci_port *p)
{
    unsigned depth = ahci_ncq_depth(p->cap, p->ncq_words);
    const char *reason = p->lba48 ? "cap.sncq/word76/depth" : "LBA48 absent";
    if (!p->lba48) depth = 0;
    for (unsigned i = 0; ahci_ncq_horkage[i]; i++) {
        const char *prefix = ahci_ncq_horkage[i];
        unsigned length = 0;
        while (prefix[length] && prefix[length] == p->model[length]) length++;
        if (!prefix[length]) {
            depth = 0;
            reason = "model horkage";
            break;
        }
    }
    p->qdepth = depth ? depth : 1;
    blk_set_queue_depth(p->block, p->qdepth);
    if (depth) {
        kprintf("[ahci] port %d: ncq on, depth %u (hba slots %u - 1 internal, drive %u) %s\n",
                p->index, depth, p->nslots, (p->ncq_words & 31) + 1, p->name);
    } else {
        kprintf("[ahci] port %d: ncq off (%s, cap.sncq=%u word76=0x%x) %s\n",
                p->index, reason, !!(p->cap & CAP_SNCQ), p->ncq_words >> 16, p->name);
    }
}

/* Bring up one controller: claim MMIO/BOHC, quiesce every firmware engine,
 * enable BME, then initialise attached SATA disks. */
static int ahci_bring_up(struct ahci_hba *dev)
{
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)dev->abar;
    g_abar = abar;

    uint32_t cap, pi, vs;
    if (ahci_prepare_controller(dev, abar, &cap, &pi, &vs)) return 0;
    if (g_nctls >= AHCI_MAX_HBA) return 0;
    struct ahci_ctl *ctl = &g_ctls[g_nctls++];
    ctl->dev = dev->model;
    ctl->reg = abar;
    ctl->pi = pi;
    int nports = (int)(cap & 0x1F) + 1;
    int ncs    = (int)((cap >> 8) & 0x1F) + 1;

    /* ncq= is new, and it is the answer to a question nothing in this tree
     * could ask before: SNCQ says whether the controller can queue at all and
     * NCS says how deep. The old comment said "the driver does not queue";
     * correction: NCQ now uses up to NCS-1 data slots after IDENTIFY confirms
     * support, reserving one internal slot. This line describes HBA capacity;
     * the per-port diagnostic below reports the negotiated device depth. */
    kprintf("[ahci] %x:%x at %02x:%02x.%d abar=%x ver=%d.%d ports=%d impl=%x slots=%d "
            "addr64=%s ncq=%s(%d slots)\n",
            dev->vendor, dev->device, (int)dev->bus, (int)dev->slot, (int)dev->func,
            (unsigned)dev->abar,
            (int)(vs >> 16), (int)((vs >> 8) & 0xFF), nports, pi, ncs,
            (cap & CAP_S64A) ? "yes" : "no",
            (cap & CAP_SNCQ) ? "yes" : "no", ncs);
    kprintf("[ahci] model/QEMU evidence only; not verified on X79-VG4 hardware\n");

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
        /* DET=1: a device is present but the PHY is not talking -- what a
         * port nobody reset can look like -- so it gets the link reset below.
         * 0 is no device; 4 is a PHY that is offline or disabled. */
        if (det != 3 && det != 1) continue;
        if (det == 3 && ipm != 1) continue;             /* not in active power state */

        uint32_t sig = r32(pr, P_SIG);
        if (det == 3 && sig != SIG_SATA && sig != 0xFFFFFFFFu) {
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
        p->controller = dev->model;
        p->ctl = ctl;
        p->index = i;
        p->reg   = pr;
        if (ahci_dma_init(p, cap)) { kprintf("[ahci] port %d: out of memory\n", i); continue; }
        memset(p->dma, 0, p->metadata->size);

        if (port_stop(p) != 0) {
            kprintf("[ahci] port %d: engines would not stop (cmd=%x) -- skipped\n", i, r32(pr, P_CMD));
            dma_free_coherent(p->metadata); p->metadata = NULL; p->dma = NULL;
            continue;
        }
        uint64_t md = dma_addr_value(p->metadata->dma);
        w32(pr, P_CLB,  (uint32_t)md);
        w32(pr, P_CLBU, (uint32_t)(md >> 32));
        w32(pr, P_FB,   (uint32_t)(md + 0x400));
        w32(pr, P_FBU,  (uint32_t)((md + 0x400) >> 32));
        w32(pr, P_SERR, r32(pr, P_SERR));
        w32(pr, P_IS,   r32(pr, P_IS));
        w32(pr, P_IE,   0);
        if (!dma_buffer_submit(p->metadata)) {
            dma_free_coherent(p->metadata); p->metadata = NULL; p->dma = NULL; continue;
        }
        if (port_needs_reset(det, sig)) {
            int rr = port_link_reset(pr, &sig);
            kprintf("[ahci] port %d: link not established by firmware (det=%u); COMRESET -> %s ssts=%x sig=%08x\n",
                    i, det, rr == 0 ? "ready" : rr == -1 ? "NO LINK" : "DEVICE NOT READY", r32(pr, P_SSTS), sig);
            if (rr != 0 || sig != SIG_SATA) {
                if (rr == 0)
                    kprintf("[ahci] port %d: %s (sig=%08x) -- skipped, not a SATA disk\n", i, sig_name(sig), sig);
                port_quiesce(p);                        /* FRE is on now: stop it before freeing FB */
                dma_free_coherent(p->metadata); p->metadata = NULL; p->dma = NULL;
                continue;
            }
        }
        if (port_start(p)) {
            kprintf("[ahci] port %d: device busy (tfd=%x) -- command engine not started, skipped\n", i, r32(pr, P_TFD));
            port_quiesce(p);
            dma_free_coherent(p->metadata); p->metadata = NULL; p->dma = NULL;
            continue;
        }

        if (port_identify(p) != 0) {
            kprintf("[ahci] port %d: IDENTIFY failed (tfd=%x serr=%x) -- skipped\n",
                    i, r32(pr, P_TFD), r32(pr, P_SERR));
            port_quiesce(p);
            dma_free_coherent(p->metadata); p->metadata = NULL; p->dma = NULL;
            continue;
        }

        p->name[0] = 'a'; p->name[1] = 'h'; p->name[2] = 'c'; p->name[3] = 'i';
        p->name[4] = (char)('0' + g_ndisks); p->name[5] = 0;
        kprintf("[ahci] port %d: SATA disk '%s' %u sectors (%u MiB) lba48=%s\n",
                i, p->model, (unsigned)p->nsectors, (unsigned)(p->nsectors / 2048),
                p->lba48 ? "yes" : "no");
        p->block = blk_register(p->name, &ahci_ops, p, p->nsectors);
        ahci_configure_queue(p);
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
#ifdef AHCI_DEVICE_MODEL
static const struct driver ahci_driver = {
    .name = "ahci", .bus_type = DEV_BUS_PCI, .remove = ahci_shutdown,
};
#endif

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
        int found = ahci_bring_up(&dev);
        total += found;
#ifdef AHCI_DEVICE_MODEL
        /* Root disks precede dev_probe_all: publish the binding now rather
         * than probing the same controller again after filesystem mounting. */
        if (found && dev.model) dev.model->drv = &ahci_driver;
#endif
    }
    return total;
}

/* Arm only after root mount and SMP. A read-only IDENTIFY witnesses actual
 * delivery, not merely the vector allocation. kmain enters with IF=0: save,
 * busy-before-sti, wait, then restore IF exactly as ahci_run does. Fifty ms is
 * a delivery diagnostic budget, not a device command timeout.
 * Not verified on X79-VG4 hardware. */
static int ahci_irq_selftest(struct ahci_ctl *c)
{
    struct ahci_port *port = NULL;
    for (int disk = 0; disk < g_ndisks; disk++) {
        if (g_ports[disk].ctl == c && g_ports[disk].metadata) {
            port = &g_ports[disk];
            break;
        }
    }
    if (!port) return -1;
    uint16_t identify[256];
    struct ahci_cmdspec spec = {ATA_IDENTIFY, 0, 0, 0, identify, sizeof identify, 0, 0};
    uint64_t flags;
    AHCI_FLAGS(flags);
    __atomic_fetch_add(&g_ata_busy, 1, __ATOMIC_RELAXED);
#ifndef AHCI_NEGCTL_SELFTEST_IF0
    AHCI_STI();
#endif
    uint32_t before = __atomic_load_n(&port->irq_events, __ATOMIC_ACQUIRE);
    uint64_t start = time_mono_ns();
    uint64_t deadline = link_ms() + 50;
    unsigned slot = port->nslots - 1;
    int issued = ahci_begin(port, slot, &spec) == 0;
    int done = 0;
    if (issued) {
        for (long spins = 0; spins < SPIN_CMD && link_ms() < deadline; spins++) {
            int result = ahci_check(port, slot);
            if (result == 1 && !done) {
                done = ahci_complete_slot(port, slot) == 0;
            }
            if (done && __atomic_load_n(&port->irq_events, __ATOMIC_ACQUIRE) != before)
                break;
            AHCI_PAUSE();
        }
    }
    uint64_t elapsed = (time_mono_ns() - start) / 1000;
    int delivered = done && __atomic_load_n(&port->irq_events, __ATOMIC_ACQUIRE) != before;
    if (delivered) {
        kprintf("[ahci] irq selftest %s: msi delivered in %llu us\n",
                port->name, (unsigned long long)elapsed);
    } else {
        kprintf("[ahci] irq selftest %s: NO INTERRUPT in 50 ms "
            "(vec=%d is=%x pxis=%x ie=%x) -> polling\n",
            port->name, c->dev->irq_vec, r32(c->reg, HBA_IS),
            r32(port->reg, P_IS), r32(port->reg, P_IE));
#ifndef AHCI_NEGCTL_NO_IRQ_FALLBACK
        for (int disk = 0; disk < g_ndisks; disk++) {
            if (g_ports[disk].ctl == c) w32(g_ports[disk].reg, P_IE, 0);
        }
        w32(c->reg, HBA_GHC, r32(c->reg, HBA_GHC) & ~GHC_IE);
        c->irq_armed = 0;
        (void)dev_irq_release(c->dev);
#endif
    }
    /* The old code called ahci_fail_port at the 50 ms diagnostic boundary,
     * permanently blocking a healthy but slow disk. Disable MSI independently,
     * then drain IDENTIFY with the normal 8000 ms/retry policy. Its DMA buffer
     * is on this stack and cannot escape, even on the polling fallback path. */
    if (issued && !done) {
        long spins = 0;
        unsigned attempt = port->slot[slot].attempt;
        for (;;) {
            int expired = spins >= SPIN_CMD || link_ms() >= port->slot[slot].deadline;
            if (ahci_step(port, slot, expired)) break;
            if (port->slot[slot].attempt != attempt) {
                attempt = port->slot[slot].attempt;
                spins = 0;
            } else {
                spins++;
            }
            AHCI_PAUSE();
        }
    }
    if (!(flags & 0x200)) AHCI_CLI();
    __atomic_fetch_sub(&g_ata_busy, 1, __ATOMIC_RELAXED);
    return delivered ? 0 : -1;
}

/* Compare sixteen disjoint 4 KiB reads against nonqueued serial reads. The
 * original serial baseline still used NCQ and its failure was only printed;
 * it could neither establish a safe fallback nor stop later queued writes.
 * Return -2 when even DMA EXT fails, so attachment offlines that port instead
 * of trusting a failed fallback. Both paths use the real block/DMA callbacks.
 * This checks queue/tag integrity, not performance on this board. max_inflight
 * counts unreaped requests: QEMU AIO may have already completed some of them.
 * Not verified on X79-VG4 hardware. */
static int ahci_ncq_selftest(struct ahci_port *p)
{
    if (p->qdepth < 2 || !p->block || p->dma_dev.blocked) return 0;
    static uint8_t queued[16][4096];
    static uint8_t serial[4096];
    struct blk_span spans[16];
    unsigned count = 0;
    for (unsigned i = 0; i < 16; i++) {
        uint64_t lba = (uint64_t)i * 2048;
        if (lba + 8 > p->nsectors) continue;
        spans[count] = (struct blk_span){lba, 8, queued[count], 0};
        count++;
    }
    if (!count) return 0;
    uint32_t saved_max = p->st.max_inflight;
    p->st.max_inflight = 0;
    uint64_t start = time_mono_ns();
    int failed = blk_dev_batch(p->block, BLK_OP_READ, spans, count);
    uint64_t queued_us = (time_mono_ns() - start) / 1000;
    uint32_t maximum = p->st.max_inflight;
    if (p->st.max_inflight < saved_max) p->st.max_inflight = saved_max;
    /* blk_dev_batch has drained every request before returning. Only now may
     * the protocol change: Linux v6.12 libata-core.c ata_std_qc_defer forbids
     * mixing NCQ and nonqueued commands. Preserve any recovery-induced depth
     * reduction rather than restoring the depth from before the queued test.
     * kmain attaches before wm_run starts the scheduler; APs still wait for
     * g_sched_ready in smp.c, so no cache writer can enter this interval. */
    unsigned depth = p->qdepth;
    p->qdepth = 1;
    blk_set_queue_depth(p->block, 1);
    int serial_failed = 0;
    start = time_mono_ns();
    for (unsigned i = 0; i < count; i++) {
        if (blk_dev_read(p->block, spans[i].lba, 8, serial)) {
            failed = 1;
            serial_failed = 1;
            continue;
        }
        for (unsigned byte = 0; byte < sizeof serial; byte++) {
            if (serial[byte] != queued[i][byte]) {
                failed = 1;
                break;
            }
        }
    }
    uint64_t serial_us = (time_mono_ns() - start) / 1000;
    kprintf("[ahci] ncq selftest %s: %u reads x 4 KiB: %llu us queued "
            "(max in flight %u unreaped requests) vs %llu us serial; data identical: %s\n",
            p->name, count, (unsigned long long)queued_us, maximum,
            (unsigned long long)serial_us, failed ? "NO" : "yes");
    /* Restore the candidate depth only for the attachment decision below;
     * no I/O occurs between this return and handling the result. */
    p->qdepth = depth;
    blk_set_queue_depth(p->block, depth);
    if (serial_failed) return -2;
    return failed ? -1 : 0;
}

/* Keep HBA IE clear throughout root mount. Late attachment is MSI-only; an
 * allocation that silently selects INTx is released rather than claimed as
 * working MSI. Source: AHCI 1.3.1 10.7; Linux v6.12 libahci.c
 * ahci_init_controller clears ports before the host latch and enables last. */
void ahci_irq_attach(void)
{
    if (!lapic_ready()) return;
    for (unsigned index = 0; index < g_nctls; index++) {
        struct ahci_ctl *ctl = &g_ctls[index];
        if (ctl->attach_tried) continue;
        int busy = 0;
        int disks = 0;
        for (int disk = 0; disk < g_ndisks; disk++) {
            if (g_ports[disk].ctl != ctl || !g_ports[disk].metadata) continue;
            disks++;
            if (g_ports[disk].active_mask) busy = 1;
        }
        if (busy) continue;
        ctl->attach_tried = 1;
        /* q35 may enumerate both the onboard HBA and a disk-bearing add-in
         * HBA. An empty controller has no IDENTIFY delivery witness. */
        if (!disks) continue;
        struct device *dev = ctl->dev;
        int available = dev && dev->cap_msi;
#ifdef AHCI_NOIRQ
        available = 0;
#endif
        if (available) {
            ctl->irq_cpu = lapic_id();
            int previous = dev_irq_prefer(DEV_IRQ_MSI);
            int vector = dev_irq_request(dev, ahci_isr, ctl, "ahci");
            dev_irq_prefer(previous);
            if (vector < 0 || dev->irq_mode != DEV_IRQ_MSI) {
                if (dev->irq_mode != DEV_IRQ_NONE) (void)dev_irq_release(dev);
                available = 0;
            }
        }
        if (available) {
            for (unsigned port = 0; port < 32; port++) {
                if (!(ctl->pi & (1u << port))) continue;
                volatile uint8_t *reg = ctl->reg + 0x100 + port * 0x80;
                w32(reg, P_IE, 0);
                w32(reg, P_IS, r32(reg, P_IS));
            }
            w32(ctl->reg, HBA_IS, r32(ctl->reg, HBA_IS));
            for (int disk = 0; disk < g_ndisks; disk++) {
                struct ahci_port *port = &g_ports[disk];
                if (port->ctl != ctl || !port->metadata) continue;
                __atomic_store_n(&port->is_latched, 0, __ATOMIC_RELEASE);
                w32(port->reg, P_IE, AHCI_PXIE_DEFAULT);
            }
            ctl->irq_armed = 1;
            w32(ctl->reg, HBA_GHC, r32(ctl->reg, HBA_GHC) | GHC_IE);
            (void)ahci_irq_selftest(ctl);
        } else {
            kprintf("[ahci] MSI unavailable -> polling\n");
        }
        for (int disk = 0; disk < g_ndisks; disk++) {
            struct ahci_port *port = &g_ports[disk];
            if (port->ctl != ctl || !port->metadata) continue;
            int result = ahci_ncq_selftest(port);
#ifndef AHCI_NEGCTL_SELFTEST_IGNORE_RESULT
            if (result < 0) {
                /* A successful status FIS cannot detect wrong data. Never
                 * admit queued writes after a failed comparison, timeout or
                 * read error. The drained DMA EXT baseline above is the only
                 * fallback witness; a failed/blocked baseline stays offline.
                 * Model/QEMU evidence, not verified on X79-VG4 hardware. */
                port->qdepth = 1;
                blk_set_queue_depth(port->block, 1);
                kprintf("[ahci] %s: ncq disabled after failed selftest; depth 1\n",
                        port->name);
                if (result == -2 || port->dma_dev.blocked) {
                    (void)ahci_fail_port(port);
                    blk_dev_offline(port->block);
                    kprintf("[ahci] %s: nonqueued fallback failed -> offline\n",
                            port->name);
                }
            }
#else
            (void)result;
#endif
            char line[192];
            if (ahci_diag(disk, line, sizeof line) >= 0)
                kprintf("[ahci] %s\n", line);
        }
    }
}

/* Bounded snapshot in the pci_intx_diag style. irq/poll_done deliberately
 * distinguish allocation from delivered interrupts on unfamiliar hardware. */
int ahci_diag(int disk, char *buf, int max)
{
    if (disk < 0 || disk >= g_ndisks || !buf || max <= 0 || !g_ports[disk].metadata)
        return -1;
    struct ahci_port *p = &g_ports[disk];
    return ksnprintf(buf, max,
                     "%s ncq=%u irq=%s/%u spur=%u poll_done=%u maxq=%u err=%u rec=%u rst=%u",
                     p->name, p->qdepth, p->ctl && p->ctl->irq_armed ? "msi" : "polled",
                     __atomic_load_n(&p->irq_events, __ATOMIC_ACQUIRE),
                     __atomic_load_n(&p->st.spurious, __ATOMIC_RELAXED),
                     p->st.poll_done, p->st.max_inflight, p->st.errors,
                     p->st.recoveries, p->st.resets);
}

/* BKL-held normal controller removal. Each port owns its DMA context, so a
 * different HBA and its in-flight requests remain unaffected. The block registry
 * retains offline objects so old filesystem/swap handles fail safely. */
void ahci_shutdown(struct device *controller)
{
    if (!controller) return;
    /* dev_unbind currently releases the vector BEFORE remove. Mask sources
     * here as well and stop using HLT, but the earlier release-to-remove window
     * is a core lifecycle risk outside this driver's ownership. */
    for (unsigned index = 0; index < g_nctls; index++) {
        struct ahci_ctl *ctl = &g_ctls[index];
        /* After the first remove, PCI decode is off. Polling controllers have
         * never enabled these sources; an already disarmed HBA needs no MMIO. */
        if (ctl->dev != controller || !ctl->irq_armed) continue;
        ctl->irq_armed = 0;
        for (unsigned port = 0; port < 32; port++) {
            if (ctl->pi & (1u << port))
                w32(ctl->reg + 0x100 + port * 0x80, P_IE, 0);
        }
        w32(ctl->reg, HBA_GHC, r32(ctl->reg, HBA_GHC) & ~GHC_IE);
    }
    /* First close all upper-layer entry points on this controller. */
    for (int i = 0; i < g_ndisks; i++) {
        struct ahci_port *p = &g_ports[i];
        if (p->controller == controller && p->metadata && p->block)
            blk_dev_offline(p->block);
    }
    for (int i = 0; i < g_ndisks; i++) {
        struct ahci_port *p = &g_ports[i];
        if (p->controller != controller || !p->metadata) continue;
        port_quiesce(p);
        for (unsigned slot = 0; slot < p->nslots; slot++) {
            ahci_unmap(p, slot);
        }
        if (dma_free_coherent(p->metadata)) panic("ahci: stopped metadata still owned");
        p->metadata = NULL; p->dma = NULL;
    }
#ifdef AHCI_DEVICE_MODEL
    dev_disable(controller);
#endif
}
