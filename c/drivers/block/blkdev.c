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

int blk_dev_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    if (!in_bounds(d, lba, count)) return -1;
    return d->ops->read(d->ctx, d->start + lba, count, buf);
}

int blk_dev_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (!in_bounds(d, lba, count)) return -1;
    return d->ops->write(d->ctx, d->start + lba, count, buf);
}

int blk_dev_flush(struct blkdev *d)
{
    if (!d || !d->ops || !d->ops->flush) return -1;
    return d->ops->flush(d->ctx);
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

static int nvme_r(void *c, uint64_t lba, uint32_t n, void *b)        { (void)c; return narrow_rw(nvme_read, 0, lba, n, b); }
static int nvme_w(void *c, uint64_t lba, uint32_t n, const void *b)  { (void)c; return narrow_rw(0, nvme_write, lba, n, (void *)b); }
static int nvme_f(void *c)                                           { (void)c; return nvme_flush(); }
static const struct blk_ops nvme_bops = { nvme_r, nvme_w, nvme_f };

static int vblk_r(void *c, uint64_t lba, uint32_t n, void *b)        { (void)c; return virtio_blk_read(lba, n, b); }
static int vblk_w(void *c, uint64_t lba, uint32_t n, const void *b)  { (void)c; return virtio_blk_write(lba, n, b); }
static int vblk_f(void *c)                                           { (void)c; return virtio_blk_flush(); }
static const struct blk_ops vblk_bops = { vblk_r, vblk_w, vblk_f };

static int ata_r(void *c, uint64_t lba, uint32_t n, void *b)         { (void)c; return narrow_rw(ata_read, 0, lba, n, b); }
static int ata_w(void *c, uint64_t lba, uint32_t n, const void *b)   { (void)c; return narrow_rw(0, ata_write, lba, n, (void *)b); }
static int ata_f(void *c)                                            { (void)c; return ata_flush(); }
static const struct blk_ops ata_bops = { ata_r, ata_w, ata_f };

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
