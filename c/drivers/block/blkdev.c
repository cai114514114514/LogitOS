#include <stdint.h>
#include <stddef.h>
#include "blkdev.h"
#include "part.h"
#include "ata.h"
#include "ahci.h"
#include "virtio_blk.h"
#include "nvme.h"
#include "kprintf.h"

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

/* --------------------------------------------------------------------------
 * The host-test seam, and it is deliberately three macros wide.
 *
 * tests/unit/blkreq_test.c compiles THIS FILE and drives the request engine
 * against fake drivers. Two things in here cannot run in ring 3 on Linux:
 * `sti`/`cli` are privileged (a plain SIGSEGV, with no hint of why), and the
 * DMA-reachability bound is the kernel's identity-mapped first gigabyte, which
 * no host malloc is anywhere near -- so on the host every buffer would take
 * the bounce path and the ordinary path would never be measured at all.
 *
 * The seam does NOT touch the engine: blk_submit, blk_poll, blk_wait, the
 * interlock and the bounce loop are the same lines in both builds. Anything
 * conditional inside those would make the gate a test of a different program.
 * ------------------------------------------------------------------------ */
#ifdef BLK_HOSTTEST
uint64_t blk_hosttest_dma_limit = ~0ull;      /* the test moves this to force a bounce */
#  define BLK_IRQ_SAVE(f)     ((f) = 0x200)
#  define BLK_IRQ_ENABLE()    ((void)0)
#  define BLK_IRQ_DISABLE()   ((void)0)
#  define BLK_RELAX()         ((void)0)
#  define BLK_DMA_LIMIT       blk_hosttest_dma_limit
#else
#  define BLK_IRQ_SAVE(f)     __asm__ volatile ("pushfq; pop %0" : "=r"(f) :: "memory")
#  define BLK_IRQ_ENABLE()    __asm__ volatile ("sti")
#  define BLK_IRQ_DISABLE()   __asm__ volatile ("cli")
#  define BLK_RELAX()         __asm__ volatile ("pause")
#  define BLK_DMA_LIMIT       (1ull << 30)    /* boot.asm:80: the identity-mapped span */
#endif

/* --------------------------------------------------------------------------
 * The registry
 * ------------------------------------------------------------------------ */

static struct blkdev g_dev[BLK_MAX_DEV];
static int           g_ndev;
static struct blkdev *g_root;

static void name_copy(char *dst, const char *src, int max)
{
    int i = 0;
    for (; src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

struct blkdev *blk_register(const char *name, const struct blk_ops *ops,
                            void *ctx, uint64_t nsectors)
{
    if (g_ndev >= BLK_MAX_DEV || !ops || !name) return NULL;
    struct blkdev *d = &g_dev[g_ndev++];
    memset(d, 0, sizeof *d);
    name_copy(d->name, name, BLK_NAME_MAX);
    d->ops = ops;
    d->ctx = ctx;
    d->start = 0;
    d->nsectors = nsectors;
    return d;
}

int             blk_count(void)      { return g_ndev; }
struct blkdev  *blk_at(int i)        { return (i >= 0 && i < g_ndev) ? &g_dev[i] : NULL; }
struct blkdev  *blk_root(void)       { return g_root; }
void            blk_set_root(struct blkdev *d) { g_root = d; }
const char     *blk_root_name(void)  { return g_root ? g_root->name : "(none)"; }

struct blkdev *blk_find(const char *name)
{
    for (int i = 0; i < g_ndev; i++)
        if (streq(g_dev[i].name, name)) return &g_dev[i];
    return NULL;
}

/* Every access goes through this bound. A partition is an offset plus a length,
 * and the length is the half that matters: without it a filesystem that
 * believes a corrupt superblock would read and WRITE straight through the end
 * of its own partition into the next one. The addition is written so it cannot
 * wrap for any 64-bit count. */
static int in_bounds(struct blkdev *d, uint64_t lba, uint32_t count)
{
    if (!d || !d->ops || count == 0) return 0;
    if (lba >= d->nsectors) return 0;
    return (uint64_t)count <= d->nsectors - lba;
}

/* --------------------------------------------------------------------------
 * DMA reachability -- the P1 fix (docs/BUG_BACKLOG.md).
 *
 * Every backend below except ATA PIO is a DMA device: virtio-blk, AHCI and
 * NVMe put the buffer address into a descriptor and the DEVICE dereferences it
 * as a PHYSICAL address. The kernel runs on the boot page tables, which
 * identity-map exactly the first 1 GiB (boot.asm:80) -- so for any buffer in
 * that range, virtual == physical and handing the pointer over is correct.
 * For anything else it is silent corruption: user images link at exactly
 * 1 GiB (MM_USER_BASE), so a user buffer handed to SYS_READ_FILE became a
 * "physical" address past the end of a 512 MiB machine's RAM. The device
 * completed fine (its status byte lives at a kernel address), the syscall
 * returned success, and the caller's buffer was never written. Writes were
 * the mirror image: garbage read from nowhere, committed to disk. It went
 * unnoticed for months because the single-block cache path happens to bounce
 * through kernel buffers -- WHILE the pool has a free slot, which is one more
 * "usually" -- and every large reader in the tree used fopen, whose F_VFS
 * backing buffer is a kmalloc and therefore identity-mapped.
 *
 * WHY THE CHECK LIVES HERE. blk_dev_read/blk_dev_write are the choke point:
 * every caller in the tree -- bcache's three direct-to-caller paths included --
 * funnels through them, and no code calls a backend directly (verified by
 * grep, and worth re-verifying if a new backend lands). Fixing the callers
 * instead means finding every present and future one; fixing the backends
 * means four copies of the same bounce.
 *
 * WHY A BOUNCE AND NOT A REFUSAL. The refusal doctrine is for requests that
 * are WRONG. This request is fine -- the caller's range was already
 * user_range_ok'd at the syscall boundary -- it is the transport that cannot
 * take the address as-is. Refusing would turn every large user-buffer read
 * into an error and fix nothing. And not a VA->phys translation either: user
 * pages are not physically contiguous, so a multi-sector run would need
 * scatter-gather that struct blk_ops does not have. The bounce is bounded,
 * allocation-free, and costs exactly the memcpy the resident-cache path
 * already pays on every hit.
 *
 * ONE static buffer USED TO BE safe because every block driver here was
 * submit-and-poll inside one call, under the BKL, so there was never a second
 * transfer in flight. That is no longer true (see blkdev.h), and the design
 * note this comment used to carry -- "this must become per-request state" --
 * has been answered the other way round, deliberately: the bounce stayed
 * single and the ASYNC PATH GAVE IT UP. blk_submit refuses an async request
 * that would need it (BLK_E_NODMA), and only blk_rw() below -- the synchronous
 * convenience, which holds the no-preemption flag across the whole chunked
 * loop -- ever touches it. Making it per-request means an allocation on the
 * page-fault path that swap.c forbids in as many words, or a pool sized by
 * guesswork; refusing costs the one caller that could ever hit it (swap, whose
 * pages are identity-mapped frames and therefore never do) a counted fallback.
 * ------------------------------------------------------------------------ */
#define BLK_BOUNCE_SECT 64u              /* 32 KiB a chunk */
static uint8_t blk_bounce[BLK_BOUNCE_SECT * BLK_SECTOR];

static int dma_reachable(const void *buf, uint32_t count)
{
    uint64_t a = (uint64_t)(uintptr_t)buf;
    uint64_t n = (uint64_t)count * BLK_SECTOR;
    return a + n >= a && a + n <= BLK_DMA_LIMIT;
}

/* --------------------------------------------------------------------------
 * The request engine
 *
 * Everything funnels through blk_submit/blk_poll: blk_dev_read is literally
 * init + submit + poll-to-completion. There is no second path, and that is the
 * property which makes the asynchrony testable rather than hopeful -- a
 * synchronous read drives the same driver state machine an asynchronous one
 * does, so an ordinary boot exercises it thousands of times before any swap
 * page is written.
 *
 * NON-PREEMPTION, AND WHY IT IS g_ata_busy. c/kernel/cpu/interrupts.c skips
 * schedule() while ata_busy() || virtio_busy() || nvme_busy(), and ahci.c
 * already shared ata.c's flag rather than adding a third, arguing it is the
 * same claim. It is now the BLOCK LAYER's flag, raised in one place, for that
 * one claim: "a synchronous block transfer this core must not be preempted out
 * of is in flight". Raising it here rather than in each driver also closes a
 * window that enabling interrupts earlier would otherwise open. schedule()
 * DROPS the BKL across a context switch (c/kernel/sched/sched.c:703) and
 * c/fs/logitfs.c relies on it never being dropped mid-operation, so there must
 * be no moment where IF is on and the flag is down. The order below is
 * flag-then-sti and never the reverse.
 *
 * An ASYNC request raises nothing. That is the entire point of it: its
 * submitter intends to give the CPU up, and the in-flight interlock -- not the
 * absence of a context switch -- is what protects the controller.
 * ------------------------------------------------------------------------ */

/* A partition shares its parent's ops and ctx, so the queue belongs to the
 * whole disk. One level is enough: this tree publishes no partition of a
 * partition (scan_partitions is called on disks only). */
static struct blkdev *medium(struct blkdev *d)
{
    return (d && d->parent) ? d->parent : d;
}

void blk_req_init(struct blk_req *r, struct blkdev *d, int op,
                  uint64_t lba, uint32_t count, void *buf)
{
    memset(r, 0, sizeof *r);
    r->dev   = d;
    r->op    = (uint8_t)op;
    r->lba   = lba;
    r->count = (op == BLK_OP_FLUSH) ? 0 : count;
    r->buf   = buf;
    r->state = BLK_REQ_IDLE;
}

/* Finish a request. The medium is released HERE and nowhere else, so there is
 * exactly one place that can leave a disk permanently busy. */
static int req_finish(struct blk_req *r, int status)
{
    struct blkdev *m = medium(r->dev);
    if (m && m->inflight == r) m->inflight = NULL;
    r->status = status;
    r->state  = BLK_REQ_DONE;
    return status;
}

/* The whole transfer through a driver that has only read/write/flush. It is
 * complete when this returns -- that is what those transports do, so saying so
 * is honest; a driver that can do better implements submit/poll and never
 * reaches here. The buffer is already known to be DMA-reachable: bouncing is
 * blk_rw()'s job, not this function's, because the single static bounce buffer
 * has to be held across a whole chunked transfer and that window belongs to
 * the caller. */
static int run_sync_ops(struct blk_req *r)
{
    struct blkdev *d = r->dev;
    uint64_t lba = d->start + r->lba;

    if (r->op == BLK_OP_FLUSH)
        return d->ops->flush ? d->ops->flush(d->ctx) : -1;
    if (r->op == BLK_OP_WRITE)
        return d->ops->write ? d->ops->write(d->ctx, lba, r->count, r->buf) : -1;
    return d->ops->read ? d->ops->read(d->ctx, lba, r->count, r->buf) : -1;
}

static unsigned long blk_nodma_refusals;
unsigned long blk_async_refusals(void) { return blk_nodma_refusals; }

int blk_poll(struct blk_req *r)
{
    if (!r) return 1;
    if (r->state != BLK_REQ_INFLIGHT) return 1;   /* DONE, or never submitted */
    struct blkdev *d = r->dev;
    if (!d->ops->poll) { req_finish(r, -1); return 1; }  /* only submit leaves INFLIGHT set */
    if (d->ops->poll(d->ctx, r) == 0) return 0;
    req_finish(r, r->status);
    return 1;
}

int blk_submit(struct blk_req *r)
{
    if (!r) return BLK_E_ARG;
    struct blkdev *d = r->dev;
    if (!d || !d->ops) return req_finish(r, BLK_E_ARG);
    if (r->op != BLK_OP_FLUSH && !in_bounds(d, r->lba, r->count))
        return req_finish(r, BLK_E_ARG);

    /* Somebody else's request is on this medium: DRIVE IT, do not wait for its
     * submitter to be scheduled (blkdev.h says why). The pointer is re-read
     * every time round because this loop can be preempted when we are the
     * async path -- while we are away another thread may finish that request
     * and start a third. */
    struct blkdev *m = medium(d);
#ifndef BLK_NO_INTERLOCK
    while (m->inflight && m->inflight != r)
        (void)blk_poll(m->inflight);
#endif

    /* A buffer the device cannot reach needs the shared bounce, which an
     * asynchronous request may not have. Refuse it AS ITSELF -- the request is
     * fine, this way of making it is not, and the caller has a correct
     * fallback. */
    if (r->op != BLK_OP_FLUSH && r->async && !dma_reachable(r->buf, r->count)) {
        blk_nodma_refusals++;
        return req_finish(r, BLK_E_NODMA);        /* medium untouched: never claimed */
    }

    r->state   = BLK_REQ_INFLIGHT;
    r->status  = 0;
    r->done    = 0;
    r->attempt = 0;
    r->dev_lba = d->start + r->lba;
    m->inflight = r;

    if (!d->ops->submit) return req_finish(r, run_sync_ops(r));

    int rc = d->ops->submit(d->ctx, r);
    if (rc < 0) return req_finish(r, rc);
    return 0;
}

int blk_wait(struct blk_req *r)
{
    if (!r) return BLK_E_ARG;
    if (r->state == BLK_REQ_DONE) return r->status;

    uint64_t fl;
    BLK_IRQ_SAVE(fl);
    g_ata_busy++;                       /* flag BEFORE sti -- see the header above */
    BLK_IRQ_ENABLE();

    if (blk_submit(r) == 0)
        while (!blk_poll(r)) BLK_RELAX();

    if (!(fl & 0x200)) BLK_IRQ_DISABLE();
    g_ata_busy--;
    return r->status;
}

/* The synchronous convenience, including the bounce. The whole chunked loop
 * runs inside ONE no-preemption window, which is what keeps the single static
 * bounce buffer this core's for the duration. */
static int blk_rw(struct blkdev *d, int op, uint64_t lba, uint32_t count, void *buf)
{
    struct blk_req r;

    if (op == BLK_OP_FLUSH) {
        blk_req_init(&r, d, BLK_OP_FLUSH, 0, 0, NULL);
        return blk_wait(&r) ? -1 : 0;
    }
    if (!in_bounds(d, lba, count)) return -1;
    if (dma_reachable(buf, count)) {
        blk_req_init(&r, d, op, lba, count, buf);
        return blk_wait(&r) ? -1 : 0;
    }

    uint64_t fl;
    BLK_IRQ_SAVE(fl);
    g_ata_busy++;
    BLK_IRQ_ENABLE();

    int rc = 0;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t done = 0; done < count && rc == 0; ) {
        uint32_t n = count - done;
        if (n > BLK_BOUNCE_SECT) n = BLK_BOUNCE_SECT;
        if (op == BLK_OP_WRITE)
            memcpy(blk_bounce, p + (size_t)done * BLK_SECTOR, (size_t)n * BLK_SECTOR);
        blk_req_init(&r, d, op, lba + done, n, blk_bounce);
        if (blk_wait(&r) != 0) { rc = -1; break; }
        if (op == BLK_OP_READ)
            memcpy(p + (size_t)done * BLK_SECTOR, blk_bounce, (size_t)n * BLK_SECTOR);
        done += n;
    }

    if (!(fl & 0x200)) BLK_IRQ_DISABLE();
    g_ata_busy--;
    return rc;
}

int blk_dev_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    return blk_rw(d, BLK_OP_READ, lba, count, buf);
}

int blk_dev_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    return blk_rw(d, BLK_OP_WRITE, lba, count, (void *)buf);
}

int blk_dev_flush(struct blkdev *d)
{
    if (!d || !d->ops) return -1;
    if (!d->ops->submit && !d->ops->flush) return -1;
    return blk_rw(d, BLK_OP_FLUSH, 0, 0, NULL);
}

/* --------------------------------------------------------------------------
 * What logitfs calls. Signatures unchanged on purpose: the filesystem does not
 * know that partitions, or several disks, or AHCI exist.
 * ------------------------------------------------------------------------ */

static int g_inited;
static void blk_ensure(void) { if (!g_inited) blk_init(); }

int blk_read(uint32_t lba, uint8_t count, void *buf)
{
    blk_ensure();
    return blk_dev_read(g_root, lba, count, buf);
}

int blk_read_n(uint64_t lba, uint32_t count, void *buf)
{
    blk_ensure();
    return blk_dev_read(g_root, lba, count, buf);
}

int blk_write(uint32_t lba, uint8_t count, const void *buf)
{
    blk_ensure();
    return blk_dev_write(g_root, lba, count, buf);
}

/* Barrier: return only once everything already written is on media.
 *
 * A completed blk_write() means the DEVICE has the data, not that the platter
 * does -- virtio-blk, NVMe and a SATA disk may all hold it in a write cache, and
 * a disk is free to reorder writes within that cache. So a journal that writes
 * its blocks, then its commit record, has ordered nothing unless a barrier
 * separates them: power loss can leave the commit record on media vouching for
 * blocks that are still in the cache and now gone. That is precisely the
 * inconsistency the journal exists to prevent, which is why an unbarriered
 * journal can be worse than none -- it asserts an order the hardware never
 * promised.
 *
 * The counter is what makes that assertion testable from outside: a test can ask
 * whether the filesystem actually issued the barriers it claims to. */
static unsigned long flush_count;

int blk_flush(void)
{
    blk_ensure();
    flush_count++;
    return blk_dev_flush(g_root);
}

unsigned long blk_flush_count(void) { return flush_count; }

/* --------------------------------------------------------------------------
 * Adapters for the drivers that predate the registry
 * ------------------------------------------------------------------------ */

/* nvme_read/ata_read take a uint32_t LBA and a uint8_t sector count, so a
 * request beyond either would silently truncate onto the WRONG SECTOR through
 * the adapters below rather than fail. Nothing in the kernel issues one today
 * (logitfs works in 8-sector blocks and the partition scanner reads one sector
 * at a time), but "nothing does today" is not a bound, so make it one. */
static int narrow_ok(uint64_t lba, uint32_t n)
{
    /* The whole request must be addressable by a 32-bit LBA, and the last
     * sector of it too -- checking only the first would let a wide request
     * start in range and finish past 2 TiB with a wrapped address. */
    return n != 0 && lba <= 0xFFFFFFFFu && (uint64_t)lba + n <= 0x100000000ull;
}

/* A request wider than the driver's own `uint8_t count`, split.
 *
 * This is an ADAPTER limit, not a device one, and it belongs here rather than
 * in the caller: bcache_read_run issues one 512 KiB read because that is what
 * makes a 3 MB file cost six device round trips instead of 741, and it must not
 * have to ask which driver holds the root disk before deciding how wide a read
 * it is allowed to want. NVMe and virtio both take the whole thing; legacy ATA
 * PIO gets it as 255-sector pieces and is still 128x better off than it was at
 * 8. */
static int narrow_rw(int (*rd)(uint32_t, uint8_t, void *),
                     int (*wr)(uint32_t, uint8_t, const void *),
                     uint64_t lba, uint32_t n, void *b)
{
    if (!narrow_ok(lba, n)) return -1;
    uint8_t *p = (uint8_t *)b;
    while (n) {
        uint32_t c = n > 255 ? 255 : n;
        int rc = rd ? rd((uint32_t)lba, (uint8_t)c, p)
                    : wr((uint32_t)lba, (uint8_t)c, p);
        if (rc) return rc;
        lba += c; n -= c; p += (size_t)c * BLK_SECTOR;
    }
    return 0;
}

/* NVMe implements the ASYNC pair and nothing else -- one implementation, so a
 * synchronous read and an asynchronous one cannot come to disagree about
 * chunking or ordering. The chunking that used to live in nvme_io (MDTS cap,
 * PRP list) is now driven across polls; see nvme.c. */
static int nvme_sub(void *c, struct blk_req *r) { (void)c; return nvme_blk_submit(r); }
static int nvme_pol(void *c, struct blk_req *r) { (void)c; return nvme_blk_poll(r); }
static const struct blk_ops nvme_bops = { .submit = nvme_sub, .poll = nvme_pol };

/* virtio-blk stays SYNCHRONOUS, and the reason is worth stating rather than
 * leaving as an omission: its completion is read out of the used ring inside
 * virtio.c's one request/poll call, which is shared with every other virtio
 * device in the tree (net, gpu, rng, balloon). Splitting it is a change to
 * c/drivers/virtio/virtio.c, which is not this file's, and the block layer
 * asks nothing of it -- a driver with only read/write is complete when submit
 * returns and the engine above says so truthfully. */
static int vblk_r(void *c, uint64_t lba, uint32_t n, void *b)        { (void)c; return virtio_blk_read(lba, n, b); }
static int vblk_w(void *c, uint64_t lba, uint32_t n, const void *b)  { (void)c; return virtio_blk_write(lba, n, b); }
static int vblk_f(void *c)                                           { (void)c; return virtio_blk_flush(); }
static const struct blk_ops vblk_bops = { .read = vblk_r, .write = vblk_w, .flush = vblk_f };

/* ATA PIO CANNOT be split, and this is the case that proves the fallback is
 * not laziness: the data crosses through the CPU's IO port, sixteen bits at a
 * time, inside the command. There is no in-flight token because there is no
 * moment at which the transfer exists without a thread driving it. */
static int ata_r(void *c, uint64_t lba, uint32_t n, void *b)         { (void)c; return narrow_rw(ata_read, 0, lba, n, b); }
static int ata_w(void *c, uint64_t lba, uint32_t n, const void *b)   { (void)c; return narrow_rw(0, ata_write, lba, n, (void *)b); }
static int ata_f(void *c)                                            { (void)c; return ata_flush(); }
static const struct blk_ops ata_bops = { .read = ata_r, .write = ata_w, .flush = ata_f };

/* --------------------------------------------------------------------------
 * Partition discovery
 * ------------------------------------------------------------------------ */

/* Sector reader handed to part.c: reads through the *device*, so it inherits
 * the bounds check and works identically for a whole disk and (in principle)
 * for a nested table inside a partition. */
static int dev_sector_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return blk_dev_read((struct blkdev *)ctx, lba, count, buf);
}

/* Does this device start with a LogitFS superblock?
 *
 * The block layer has no business knowing a filesystem's magic number, and on a
 * real OS this would be a `root=` boot parameter instead. It is here because
 * something has to decide which of several partitions to mount and nothing else
 * in the boot path is in a position to: the VFS is handed one device, already
 * chosen. The constants are duplicated from c/fs/logitfs.c (and tools/mkfs.py)
 * rather than shared, because the alternative is the block layer including a
 * filesystem header, which is the worse dependency of the two. */
#define LOGITFS_MAGIC   0x4C4F4749u   /* "LOGI" */
#define LOGITFS_VERSION 4
#define LOGITFS_BS      4096

static int has_logitfs(struct blkdev *d)
{
    uint8_t sec[BLK_SECTOR];
    if (blk_dev_read(d, 0, 1, sec) != 0) return 0;
    const uint32_t *w = (const uint32_t *)(const void *)sec;
    return w[0] == LOGITFS_MAGIC && w[1] == LOGITFS_VERSION && w[2] == LOGITFS_BS;
}

static void mib(uint64_t sectors, unsigned *whole, unsigned *frac)
{
    /* sectors * 512 / 2^20, to one decimal, without floating point. */
    *whole = (unsigned)(sectors / 2048);
    *frac  = (unsigned)(((sectors % 2048) * 10) / 2048);
}

static const char *mbr_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E: return "FAT";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    default:   return "unknown";
    }
}

/* Read one disk's table and publish each partition as a device named <disk>p<n>.
 * Partitions inherit the parent's ops and ctx with an absolute start LBA, so a
 * partition read is one addition, not a chain of forwarding calls. */
static void scan_partitions(struct blkdev *disk)
{
    static struct part_table t;      /* ~1.4 KiB: too big for a kernel stack frame */

    if (part_scan(dev_sector_read, disk, disk->nsectors, &t) < 0) return;
    disk->scheme = t.scheme;

    if (t.scheme == PART_NONE) {
        kprintf("[part] %s: no partition table%s\n", disk->name,
                has_logitfs(disk) ? " (raw filesystem image)" : "");
        return;
    }

    kprintf("[part] %s: %s%s%s -- %d partition%s, %d rejected%s%s\n",
            disk->name, part_scheme_name(t.scheme),
            t.protective ? " (protective MBR)" : "",
            t.backup_used ? " [PRIMARY HEADER BAD, backup used]" : "",
            t.count, t.count == 1 ? "" : "s", t.skipped,
            t.overlaps ? ", OVERLAPPING ENTRIES" : "",
            t.truncated ? ", more than we can hold" : "");

    for (int i = 0; i < t.count; i++) {
        struct part_entry *e = &t.e[i];
        char pname[BLK_NAME_MAX];
        int n = 0;
        for (; disk->name[n] && n < BLK_NAME_MAX - 4; n++) pname[n] = disk->name[n];
        pname[n++] = 'p';
        if (i + 1 >= 10) pname[n++] = (char)('0' + (i + 1) / 10);
        pname[n++] = (char)('0' + (i + 1) % 10);
        pname[n] = 0;

        struct blkdev *p = blk_register(pname, disk->ops, disk->ctx, e->count);
        if (!p) { kprintf("[part] %s: device table full\n", disk->name); return; }
        p->parent     = disk;
        p->start      = disk->start + e->start;
        p->part_index = i + 1;
        p->type_mbr   = e->type_mbr;
        if (t.scheme == PART_GPT) name_copy(p->label, e->name, BLK_LABEL_MAX);
        else                      name_copy(p->label, mbr_type_name(e->type_mbr), BLK_LABEL_MAX);

        unsigned w, f;
        mib(e->count, &w, &f);
        if (t.scheme == PART_GPT) {
            char guid[37];
            part_guid_str(e->type_guid, guid);
            kprintf("[part]   %s lba %u..%u  %u.%u MiB  type %s  name '%s'%s\n",
                    p->name, (unsigned)e->start, (unsigned)(e->start + e->count - 1),
                    w, f, guid, p->label, e->bootable ? "  [boot]" : "");
        } else {
            kprintf("[part]   %s lba %u..%u  %u.%u MiB  type %02x (%s)%s%s\n",
                    p->name, (unsigned)e->start, (unsigned)(e->start + e->count - 1),
                    w, f, e->type_mbr, p->label,
                    e->logical ? "  [logical]" : "", e->bootable ? "  [boot]" : "");
        }
    }
}

/* --------------------------------------------------------------------------
 * Bring-up
 * ------------------------------------------------------------------------ */

/* Order matters twice over. It is the order the boot log reads in, and it is
 * the order the root filesystem is looked for in -- so NVMe and virtio-blk stay
 * ahead of AHCI, which keeps `make test` and `make test-nvme` selecting exactly
 * the device they selected before this registry existed. A whole disk is
 * probed before its own partitions for the same reason: a raw filesystem image
 * written straight to the device, which is what every harness here builds, must
 * still win over anything a stray signature in it might look like. */
static void announce(struct blkdev *d, const char *kind)
{
    unsigned w, f;
    mib(d->nsectors, &w, &f);
    kprintf("[blk] %s: %s, %u sectors (%u.%u MiB)\n", d->name, kind, (unsigned)d->nsectors, w, f);
}

void blk_init(void)
{
    if (g_inited) return;
    g_inited = 1;                /* set FIRST: the probes below issue blk_dev_read
                                  * through the lazy path and must not recurse */
    int disks = 0;

    if (nvme_present()) {
        struct blkdev *d = blk_register("nvme0", &nvme_bops, NULL, nvme_capacity());
        if (d) { disks++; announce(d, "NVMe namespace"); scan_partitions(d); }
    }
    if (virtio_blk_present()) {
        struct blkdev *d = blk_register("vblk0", &vblk_bops, NULL, virtio_blk_capacity());
        if (d) { disks++; announce(d, "virtio-blk"); scan_partitions(d); }
    }

    /* AHCI is probed here rather than from kmain because it is the block layer
     * that knows what to do with what it finds. It registers its own ports. */
    int first_ahci = g_ndev;
    ahci_init();
    int last_ahci = g_ndev;
    for (int i = first_ahci; i < last_ahci; i++) {
        disks++;
        announce(&g_dev[i], "AHCI SATA disk");
        scan_partitions(&g_dev[i]);
    }

    uint64_t ata_sectors = 0;
    char ata_model[41];
    if (ata_identify(&ata_sectors, ata_model) == 0 && ata_sectors) {
        struct blkdev *d = blk_register("ata0", &ata_bops, NULL, ata_sectors);
        if (d) {
            disks++;
            kprintf("[ata] primary master '%s'\n", ata_model);
            announce(d, "legacy ATA PIO");
            scan_partitions(d);
        }
    }

    /* Root selection. Registration order is disk, then that disk's partitions,
     * then the next disk -- so this walk asks "raw nvme0? nvme0p1? nvme0p2?
     * raw vblk0? ..." which is the priority we want without a second sort. */
    for (int i = 0; i < g_ndev && !g_root; i++)
        if (has_logitfs(&g_dev[i])) g_root = &g_dev[i];

    if (!g_root && g_ndev) {
        /* Nothing carried a superblock. Fall back to the highest-priority whole
         * disk so the mount fails the same way it always did -- with "[fs] mount
         * FAILED" against a real device -- rather than with no device at all,
         * which is a different and much more confusing symptom. */
        for (int i = 0; i < g_ndev; i++)
            if (!g_dev[i].parent) { g_root = &g_dev[i]; break; }
    }

    int nparts = g_ndev - disks;
    kprintf("[blk] %d disk%s, %d partition%s; root = %s%s\n",
            disks, disks == 1 ? "" : "s", nparts, nparts == 1 ? "" : "s",
            blk_root_name(),
            g_root && has_logitfs(g_root) ? " (LogitFS superblock)" : " (NO FILESYSTEM FOUND)");
}
