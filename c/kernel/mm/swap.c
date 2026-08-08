#include <stdint.h>
#include <stddef.h>
#include "swap.h"
#include "pmm.h"
#include "mmhost.h"
#include "spinlock.h"
#include "kprintf.h"

#ifdef MM_HOSTTEST
/* The host tests drive the real slot allocator and the real PTE/round-trip
 * logic over a RAM-backed "device" the test owns (mm_common.c). Only the two
 * calls that touch hardware are replaced; everything above them is the code the
 * kernel runs. */
int  swap_host_dev(uint64_t *nsectors, const char **name);
int  swap_host_read(uint64_t lba, uint32_t n, void *buf);
int  swap_host_write(uint64_t lba, uint32_t n, const void *buf);
static inline uint64_t sw_cyc(void) { return 0; }
static inline void sw_park(void) { }
#else
#include "blkdev.h"
#include "sched.h"          /* bkl_hlt_wait(): wait WITHOUT holding the BKL */
#include "percpu.h"         /* this_cpu()->in_kernel: do we hold it in the first place? */
static inline uint64_t sw_cyc(void)
{ uint32_t lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo; }

/* Give the CPU up while another transfer finishes.
 *
 * bkl_hlt_wait() RELEASES the big kernel lock, halts, and re-acquires -- which
 * is exactly right when the caller holds it, and catastrophic when it does not:
 * it would unlock a lock this core never took, handing the kernel to two
 * threads at once. Reclaim is reachable from pmm_alloc(), pmm_alloc() is
 * reachable from allocations made by syscalls that run BKL-free
 * (syscall_is_bkl_free), so "the fault path always holds the BKL" is true of the
 * fault path and not of every caller. `in_kernel` is the flag the scheduler
 * already keeps for precisely this question, so ask it rather than assume. */
static inline void sw_park(void)
{
    if (this_cpu()->in_kernel) bkl_hlt_wait();
    else __asm__ volatile ("pause");
}
#endif

void *memset(void *, int, size_t);

/* A 1 GiB cap on the swap area, so the refcount table is bounded (2 bytes a
 * slot -> 512 KiB at the cap). Swap bigger than RAM by more than a factor of
 * two buys thrashing, not capacity. */
#define SWAP_MAX_SLOTS 262144u

static spinlock_t swap_lock = SPINLOCK_INIT;

static int       sw_ready;
static uint16_t *sw_ref;                 /* per slot; 0 = free. Index 0 unused. */
static uint64_t  sw_nslots;              /* highest valid slot number */
static uint64_t  sw_used, sw_hint = 1;
static uint64_t  sw_writes, sw_reads, sw_errors, sw_waits;
static uint64_t  sw_bkl_cyc, sw_bkl_worst;
static const char *sw_name = "(none)";

#ifndef MM_HOSTTEST
static struct blkdev *sw_dev;
#endif

/* One in-flight transfer. Not a mutex from c/kernel/core/wait.h on purpose: the
 * callers are page-fault handlers, and the wait a fault is allowed to do is the
 * one the scheduler already sanctions from trap context -- bkl_hlt_wait(), which
 * drops the BKL, halts until an interrupt, and re-takes it. See swap_io(). */
static volatile int sw_busy;

/* --------------------------------------------------------------- device -- */

struct swap_hdr {
    uint32_t magic, version, page_size, reserved;
    uint64_t nslots;
    uint64_t boot_id;
};

static int dev_read(uint64_t lba, uint32_t n, void *buf)
{
#ifdef MM_HOSTTEST
    return swap_host_read(lba, n, buf);
#else
    return blk_dev_read(sw_dev, lba, n, buf);
#endif
}

static int dev_write(uint64_t lba, uint32_t n, const void *buf)
{
#ifdef MM_HOSTTEST
    return swap_host_write(lba, n, buf);
#else
    return blk_dev_write(sw_dev, lba, n, buf);
#endif
}

#ifndef MM_HOSTTEST
/* Would writing to `d` reach the same medium as the root filesystem? A
 * partition and its parent share a medium, and so do two partitions of one
 * disk. Getting this wrong is the failure this whole file is most afraid of. */
static int touches_root(struct blkdev *d)
{
    struct blkdev *r = blk_root();
    if (!r || !d) return 1;                    /* no root known: refuse everything */
    if (d == r) return 1;
    if (d->parent && d->parent == r) return 1;
    if (r->parent && r->parent == d) return 1;
    if (d->parent && r->parent && d->parent == r->parent) return 1;
    return 0;
}

static int sector_is_blank(const uint8_t *s)
{
    for (int i = 0; i < 512; i++) if (s[i]) return 0;
    return 1;
}

static int looks_like_logitfs(const uint8_t *s)
{
    const uint32_t *w = (const uint32_t *)(const void *)s;
    return w[0] == 0x4C4F4749u;               /* "LOGI" -- see blkdev.c */
}

/* Choose a device. Every rejection says why: a machine that boots without swap
 * because a disk looked wrong must say which disk and which rule. */
static struct blkdev *pick_device(void)
{
    static uint8_t sec[512];
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_at(i);
        if (!d || !d->ops) continue;
        if (touches_root(d)) continue;                        /* rule 1 + 2, silent: expected */
        if (d->nsectors < SWAP_HDR_SECTORS + SWAP_SECTORS) {
            kprintf("[swap] %s: too small (%d sectors)\n", d->name, (int)d->nsectors);
            continue;
        }
        if (blk_dev_read(d, 0, 1, sec) != 0) {
            kprintf("[swap] %s: sector 0 unreadable\n", d->name);
            continue;
        }
        if (looks_like_logitfs(sec)) {
            kprintf("[swap] %s: carries a LogitFS superblock -- REFUSED\n", d->name);
            continue;
        }
        const struct swap_hdr *h = (const struct swap_hdr *)(const void *)sec;
        if (h->magic == SWAP_MAGIC) return d;                 /* ours from a previous boot */
        if (sector_is_blank(sec)) return d;                   /* rule 4: blank, safe to claim */
        kprintf("[swap] %s: sector 0 is not blank and not a swap header -- REFUSED "
                "(first bytes %02x %02x %02x %02x)\n",
                d->name, sec[0], sec[1], sec[2], sec[3]);
    }
    return NULL;
}
#endif

int swap_ready(void) { return sw_ready; }
const char *swap_dev_name(void) { return sw_name; }

int swap_init(void)
{
    if (sw_ready) return 0;

    uint64_t nsectors = 0;
#ifdef MM_HOSTTEST
    if (!swap_host_dev(&nsectors, &sw_name)) return -1;
#else
    sw_dev = pick_device();
    if (!sw_dev) {
        kprintf("[swap] no eligible device -- swap is OFF (reclaim keeps the drop tier)\n");
        return -1;
    }
    sw_name = sw_dev->name;
    nsectors = sw_dev->nsectors;
#endif

    uint64_t slots = (nsectors - SWAP_HDR_SECTORS) / SWAP_SECTORS;
    if (slots > SWAP_MAX_SLOTS) slots = SWAP_MAX_SLOTS;
    if (slots == 0) return -1;

    /* The refcount table comes from the PMM once, here, and never again. Every
     * later swap operation runs on a path that is trying to free memory and may
     * therefore not allocate any -- see reclaim.h. */
    uint64_t bytes = (slots + 1) * sizeof(uint16_t);
    uint64_t frames = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
    uint64_t base = pmm_alloc_contig((size_t)frames);
    if (!base) {
        kprintf("[swap] cannot reserve %d frames for the slot table -- swap is OFF\n",
                (int)frames);
        return -1;
    }
    sw_ref = (uint16_t *)mm_p2v(base);
    memset(sw_ref, 0, (size_t)(frames * FRAME_SIZE));
    sw_nslots = slots;

    /* Rewrite the header. Swap is never carried across a boot (swap.h says why),
     * so this is a claim of ownership, not a resume. */
    static uint8_t sec[512];
    memset(sec, 0, sizeof sec);
    struct swap_hdr *h = (struct swap_hdr *)(void *)sec;
    h->magic = SWAP_MAGIC;
    h->version = SWAP_VERSION;
    h->page_size = SWAP_PAGE_SIZE;
    h->nslots = slots;
    h->boot_id = sw_cyc();
    if (dev_write(0, 1, sec) != 0) {
        kprintf("[swap] %s: header write failed -- swap is OFF\n", sw_name);
        return -1;
    }

    sw_ready = 1;
    kprintf("[swap] %s: %d slots of %d bytes (%d MiB), slot table %d KiB\n",
            sw_name, (int)slots, SWAP_PAGE_SIZE,
            (int)(slots * SWAP_PAGE_SIZE / (1024 * 1024)), (int)(bytes / 1024));
    return 0;
}

/* ----------------------------------------------------------------- slots -- */

uint64_t swap_alloc_slot(void)
{
    if (!sw_ready) return SWAP_NOSLOT;
    uint64_t got = SWAP_NOSLOT;
    uint64_t fl = spin_lock_irqsave(&swap_lock);
    for (uint64_t pass = 0; pass < 2 && !got; pass++) {
        uint64_t from = pass ? 1 : sw_hint;
        uint64_t to   = pass ? sw_hint : sw_nslots + 1;
        for (uint64_t s = from; s < to; s++)
            if (sw_ref[s] == 0) { sw_ref[s] = 1; sw_used++; sw_hint = s + 1; got = s; break; }
    }
    if (sw_hint > sw_nslots) sw_hint = 1;
    spin_unlock_irqrestore(&swap_lock, fl);
    return got;
}

void swap_slot_ref(uint64_t slot)
{
    if (!sw_ready || slot == 0 || slot > sw_nslots) return;
    uint64_t fl = spin_lock_irqsave(&swap_lock);
    if (sw_ref[slot] == 0)
        kprintf("[swap] BUG: reference taken on free slot %d\n", (int)slot);
    else if (sw_ref[slot] < 0xFFFFu)
        sw_ref[slot]++;
    spin_unlock_irqrestore(&swap_lock, fl);
}

void swap_slot_put(uint64_t slot)
{
    if (!sw_ready || slot == 0 || slot > sw_nslots) return;
    uint64_t fl = spin_lock_irqsave(&swap_lock);
    if (sw_ref[slot] == 0) {
        kprintf("[swap] BUG: double free of slot %d\n", (int)slot);
    } else if (--sw_ref[slot] == 0) {
        sw_used--;
        /* The slot's bytes are left on the device. Nothing can read them: a
         * slot is only reachable through a PTE, and the PTE that pointed here
         * is gone. Zeroing would cost a 4 KiB write on the path that is trying
         * to make memory cheaper. */
    }
    spin_unlock_irqrestore(&swap_lock, fl);
}

unsigned swap_slot_refs(uint64_t slot)
{
    if (!sw_ready || slot == 0 || slot > sw_nslots) return 0;
    uint64_t fl = spin_lock_irqsave(&swap_lock);
    unsigned r = sw_ref[slot];
    spin_unlock_irqrestore(&swap_lock, fl);
    return r;
}

/* -------------------------------------------------------------------- io -- */

/* WHAT A SWAP TRANSFER DOES TO THE REST OF THE MACHINE.
 *
 * A page fault that needs the disk is the one place where memory management and
 * the scheduler have to agree, and getting it wrong is not subtle: if the fault
 * handler holds the BKL and spins on the device, then for the whole duration of
 * every swap-in nothing else in the kernel runs on any core. On a thrashing
 * machine that is the difference between "slow" and "stopped".
 *
 * There are two waits here and they are not the same wait.
 *
 *   THE QUEUEING WAIT -- waiting for the device to be free of the PREVIOUS
 *   swap transfer. This one is potentially long (as long as another page's
 *   whole transfer) and it is done with bkl_hlt_wait(), which drops the BKL,
 *   halts until the next interrupt, and re-acquires. So a second faulting
 *   thread costs the machine nothing while it waits: other cores keep working,
 *   the timer keeps ticking, the window manager keeps drawing. This is the wait
 *   that matters for "does the machine stay alive under pressure", and it is
 *   the one the scheduler explicitly sanctions from trap context.
 *
 *   THE TRANSFER ITSELF -- one 4 KiB read or write. This still runs with the
 *   BKL held, because every block driver in the tree is synchronous: blk_dev_read
 *   submits and polls for completion inside one call, with no way to give the
 *   CPU back in the middle. Splitting it is not a change this line can make --
 *   c/drivers/block/ belongs to the filesystem-durability line, and its own
 *   ahci.c says why a naive sleep there corrupts logitfs's static staging
 *   buffers.
 *
 * So the transfer is MEASURED instead of assumed: swap_bkl_cycles() is the
 * total and swap_bkl_worst() the worst single one, both printed by
 * swap_report(), so the cost of the remaining gap is a number in the log rather
 * than a paragraph in a comment.
 *
 * WHAT WOULD CLOSE IT, stated precisely so it can be asked for: two more
 * function pointers on `struct blk_ops` --
 *
 *     int (*submit)(void *ctx, uint64_t lba, uint32_t n, void *buf, int write);
 *     int (*poll)(void *ctx);        // 1 = complete, 0 = still running
 *
 * With those, this function becomes submit(); while (!poll()) bkl_hlt_wait();
 * and the BKL is not held across any part of a swap transfer. Nothing else here
 * changes. */
static int swap_io(uint64_t slot, void *page, int write)
{
    if (!sw_ready || slot == 0 || slot > sw_nslots) return -1;

    /* Claim the device. The test-and-set is under the lock; the WAIT is not,
     * because waiting means giving the BKL back. */
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&swap_lock);
        if (!sw_busy) { sw_busy = 1; spin_unlock_irqrestore(&swap_lock, fl); break; }
        spin_unlock_irqrestore(&swap_lock, fl);
        sw_waits++;
        sw_park();                   /* BKL released for the duration of the wait */
    }

    uint64_t lba = SWAP_HDR_SECTORS + (slot - 1) * SWAP_SECTORS;
    uint64_t t0 = sw_cyc();
    int rc = write ? dev_write(lba, SWAP_SECTORS, page)
                   : dev_read(lba, SWAP_SECTORS, page);
    uint64_t dt = sw_cyc() - t0;

    uint64_t fl = spin_lock_irqsave(&swap_lock);
    sw_bkl_cyc += dt;
    if (dt > sw_bkl_worst) sw_bkl_worst = dt;
    if (rc) sw_errors++;
    else if (write) sw_writes++;
    else sw_reads++;
    sw_busy = 0;
    spin_unlock_irqrestore(&swap_lock, fl);
    return rc;
}

int swap_write_page(uint64_t slot, const void *page) { return swap_io(slot, (void *)page, 1); }
int swap_read_page(uint64_t slot, void *page)        { return swap_io(slot, page, 0); }

/* ---------------------------------------------------------- accounting -- */

uint64_t swap_slots_total(void) { return sw_nslots; }
uint64_t swap_slots_used(void)  { return sw_used; }
uint64_t swap_writes(void)      { return sw_writes; }
uint64_t swap_reads(void)       { return sw_reads; }
uint64_t swap_io_errors(void)   { return sw_errors; }
uint64_t swap_waits(void)       { return sw_waits; }
uint64_t swap_bkl_cycles(void)  { return sw_bkl_cyc; }
uint64_t swap_bkl_worst(void)   { return sw_bkl_worst; }

void swap_report(const char *tag)
{
    if (!sw_ready) {
        kprintf("[swap] %s: OFF (no device)\n", tag ? tag : "-");
        return;
    }
    uint64_t ios = sw_writes + sw_reads;
    kprintf("[swap] %s: %s, %d/%d slots used (%d MiB), %d out, %d in, "
            "%d errors, %d queue waits\n",
            tag ? tag : "-", sw_name, (int)sw_used, (int)sw_nslots,
            (int)(sw_used * SWAP_PAGE_SIZE / (1024 * 1024)),
            (int)sw_writes, (int)sw_reads, (int)sw_errors, (int)sw_waits);
    kprintf("[swap] %s: BKL held inside the device: %d kcycles total, "
            "%d per transfer, %d worst\n",
            tag ? tag : "-", (int)(sw_bkl_cyc / 1000),
            (int)(ios ? sw_bkl_cyc / ios : 0), (int)sw_bkl_worst);
}
