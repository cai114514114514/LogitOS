#include <stdint.h>
#include <stddef.h>
#include "fsck.h"
#include "crc32.h"
#include "kheap.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* ------------------------------------------------------------------------- */
/* geometry                                                                   */
/* ------------------------------------------------------------------------- */

/* The bounds every consumer of an untrusted superblock needs. These were
 * originally inline in logitfs_mount; they live here so the mounted and the
 * offline view of "usable image" are literally the same code. Each check exists
 * because the value it bounds is later used to size an allocation or to index
 * one, and all of them come off the disk. */
int fsck_super_valid(const struct lfs_super *sb)
{
    if (!sb) return -1;
    if (sb->magic != LFS_MAGIC || sb->version != LFS_VERSION) return -1;
    if (sb->block_size != LFS_BS) return -1;
    /* bitmap_blocks * BS sizes an allocation: a crafted value wraps it small
     * and the read loop then writes BS bytes per block past the end. */
    if (sb->bitmap_blocks == 0 || sb->bitmap_blocks > 1024) return -1;
    if (sb->inode_blocks  == 0 || sb->inode_blocks  > 4096) return -1;
    /* inode_count bounds iget()/ialloc() indexing but the buffer is sized by
     * inode_blocks -- tie them together or a crafted count walks off the end. */
    if (sb->inode_count == 0 ||
        sb->inode_count > (uint32_t)sb->inode_blocks * LFS_IPB) return -1;
    /* total_blocks indexes the free bitmap; past its bit coverage every
     * allocation is an out-of-bounds heap write. */
    if (sb->total_blocks == 0 ||
        sb->total_blocks > (uint32_t)sb->bitmap_blocks * LFS_BS * 8) return -1;
    /* Regions inside the image, in mkfs order, so they cannot overlap each
     * other or the superblock in block 0. 64-bit sums: a crafted start must not
     * wrap the comparison. */
    if (sb->bitmap_start == 0) return -1;
    if ((uint64_t)sb->bitmap_start + sb->bitmap_blocks > sb->total_blocks) return -1;
    if ((uint64_t)sb->inode_start  + sb->inode_blocks  > sb->total_blocks) return -1;
    if (sb->data_start > sb->total_blocks) return -1;
    if (sb->inode_start < sb->bitmap_start + sb->bitmap_blocks) return -1;
    if (sb->data_start  < sb->inode_start  + sb->inode_blocks)  return -1;
    /* The log sits between the inode table and the data area, with a header
     * block and at least one body slot, within the driver's static cap. */
    if (sb->log_blocks < 2 || sb->log_blocks - 1 > LFS_LOG_MAX) return -1;
    if (sb->log_start < sb->inode_start + sb->inode_blocks) return -1;
    if ((uint64_t)sb->log_start + sb->log_blocks > sb->data_start) return -1;
    if (sb->root_ino >= sb->inode_count) return -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* the write-ahead log                                                        */
/* ------------------------------------------------------------------------- */

uint32_t lfs_log_hdr_crc(const void *hdr_block)
{
    return crc32(hdr_block, LFS_BS - 4);
}

/* Is this header a commit record for the bodies that follow it?
 *
 * Two independent checks, and each one rules out a different crash:
 *
 *   hcrc  the header block itself landed whole. A block write that tore leaves
 *         a header whose magic and count may well still look plausible -- this
 *         is the "truncated final record" every journal has to reject, and
 *         nothing but a checksum over the record can see it.
 *
 *   bcrc  the bodies in the log slots are the ones this header describes. This
 *         is the check that closes the STALE HEADER window: the previous
 *         transaction's header is cleared at the end of its commit, but that
 *         clear is a device write like any other, so the next transaction's
 *         body writes can reach media while the clear has not. Recovery would
 *         then find transaction N's header (perfectly valid, correct hcrc)
 *         standing over transaction N+1's bodies, and install N+1's data at N's
 *         target block numbers. The CRC over the bodies makes that combination
 *         unrepresentable: a header only ever applies to the bytes it was
 *         sealed against.
 *
 * Returns the block count on success, 0 for "nothing to replay", -1 for a
 * header that is sealed but names a target we refuse to write. */
static int log_validate(struct fsck_dev *dev, const struct lfs_super *sb,
                        uint32_t *hdr, uint8_t *scratch)
{
    if (hdr[LFS_LOGH_MAGIC] != LFS_LOG_MAGIC) return 0;
    if (hdr[LFS_LOGH_HCRC]  != lfs_log_hdr_crc(hdr)) return 0;   /* torn header */

    uint32_t n = hdr[LFS_LOGH_COUNT];
    if (n == 0 || n > sb->log_blocks - 1) return 0;
    if (n > LFS_LOG_MAX) return 0;
    if (LFS_LOGH_TARGET + n > LFS_LOGH_HCRC) return 0;           /* targets would overrun */

    /* Every target must be a block we are willing to overwrite. A forged or
     * corrupt header must not be able to aim recovery at the superblock, at the
     * log itself, or past the end of the image. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = hdr[LFS_LOGH_TARGET + i];
        if (t == 0 || t >= sb->total_blocks) return -1;
        if (t >= sb->log_start && t < sb->log_start + sb->log_blocks) return -1;
    }

    uint32_t crc = CRC32_INIT;
    for (uint32_t i = 0; i < n; i++) {
        if (dev->read(dev->ctx, sb->log_start + 1 + i, scratch)) return -1;
        crc = crc32_update(crc, scratch, LFS_BS);
    }
    if (crc32_final(crc) != hdr[LFS_LOGH_BCRC]) return 0;        /* bodies are not ours */
    return (int)n;
}

int fsck_log_recover(struct fsck_dev *dev, const struct lfs_super *sb, int *discarded)
{
    uint8_t *hdrb = kmalloc(LFS_BS), *body = kmalloc(LFS_BS);
    int installed = -1;
    if (discarded) *discarded = 0;
    if (!hdrb || !body) goto out;
    if (dev->read(dev->ctx, sb->log_start, hdrb)) goto out;

    uint32_t *h = (uint32_t *)(void *)hdrb;
    int sealed = (h[LFS_LOGH_MAGIC] == LFS_LOG_MAGIC);
    int n = log_validate(dev, sb, h, body);
    if (n < 0) { installed = -1; goto out; }

    if (n == 0) {
        /* Not a commit record. If the block nonetheless carries the magic it is
         * a torn or stale one: clear it so the next mount does not re-examine
         * it, and so nothing later mistakes it for a record. */
        installed = 0;
        if (sealed) {
            if (discarded) *discarded = 1;
            if (dev->write) {
                memset(hdrb, 0, LFS_BS);
                if (dev->write(dev->ctx, sb->log_start, hdrb)) installed = -1;
            }
        }
        goto out;
    }

    if (!dev->write) { installed = n; goto out; }   /* read-only: report only */

    for (int i = 0; i < n; i++) {
        if (dev->read(dev->ctx, sb->log_start + 1 + (uint32_t)i, body)) goto out;
        if (dev->write(dev->ctx, h[LFS_LOGH_TARGET + i], body)) goto out;
    }
    /* The install must be on media before the header is cleared -- otherwise a
     * second crash here loses the transaction recovery just restored, and the
     * log no longer says how to restore it again. */
    if (dev->sync && dev->sync(dev->ctx)) goto out;
    memset(hdrb, 0, LFS_BS);
    if (dev->write(dev->ctx, sb->log_start, hdrb)) goto out;
    installed = n;
out:
    kfree(hdrb); kfree(body);
    return installed;
}

/* ------------------------------------------------------------------------- */
/* the full check                                                             */
/* ------------------------------------------------------------------------- */

struct ck {
    struct fsck_dev   *dev;
    struct lfs_super   sb;
    struct fsck_report *r;
    fsck_say            say;
    void               *saycx;
    int                 repair;

    uint8_t  *seen;            /* bit per block: referenced by some inode */
    uint8_t  *bitmap;          /* the on-disk free bitmap, in RAM */
    struct lfs_dinode *inodes; /* the whole inode table, in RAM */
    uint8_t  *nlink;           /* dirents naming each inode (saturating at 255) */
    uint8_t  *visited;         /* directories already walked (loop detection) */
    int       inodes_dirty, bitmap_dirty;
};

static void note(struct ck *c, const char *msg, uint32_t a, uint32_t b)
{
    c->r->problems++;
    if (c->say) c->say(c->saycx, msg, a, b);
}

static int  bget(const uint8_t *m, uint32_t b) { return (m[b >> 3] >> (b & 7)) & 1; }
static void bset(uint8_t *m, uint32_t b)       { m[b >> 3] |=  (uint8_t)(1u << (b & 7)); }
static void bclr(uint8_t *m, uint32_t b)       { m[b >> 3] &= (uint8_t)~(1u << (b & 7)); }

/* Claim a data block for an inode. Out-of-range and double-claims are the two
 * findings; a double claim is the one fsck must NOT try to fix, because the
 * only fixes are "give it to A" and "give it to B" and nothing on the disk says
 * which. Reported, and the bitmap bit is set so at least neither copy is handed
 * out again. */
static int claim_block(struct ck *c, uint32_t blk, uint32_t ino)
{
    if (blk < c->sb.data_start || blk >= c->sb.total_blocks) {
        c->r->bad_blockptr++;
        note(c, "inode %u: block pointer %u outside the data area", ino, blk);
        return -1;
    }
    if (bget(c->seen, blk)) {
        c->r->dup_block++;
        note(c, "block %u is claimed by inode %u AND another inode "
                "(NOT repairable: nothing says which)", blk, ino);
        return -1;
    }
    bset(c->seen, blk);
    return 0;
}

/* Walk one inode's block chain, claiming every data and pointer block.
 * Returns the number of leading data blocks that are actually mapped, which is
 * what bounds a believable size. */
static uint32_t walk_inode(struct ck *c, uint32_t ino, uint8_t *ind, uint8_t *l2)
{
    struct lfs_dinode *in = &c->inodes[ino];
    uint32_t want = (in->size + LFS_BS - 1) / LFS_BS;
    uint32_t got  = 0;

    for (uint32_t i = 0; i < LFS_NDIRECT; i++) {
        if (!in->direct[i]) break;
        if (claim_block(c, in->direct[i], ino)) return got;
        got++;
    }
    if (got < LFS_NDIRECT || !in->indirect) return got;

    /* `ind` is reused for the single-indirect block and then for the
     * double-indirect L1: they are never needed at the same time, and one
     * kmalloc'd block per level is what keeps this off a kernel stack. */
    const uint32_t *lvl = (const uint32_t *)(const void *)ind;
    if (claim_block(c, in->indirect, ino)) return got;
    if (c->dev->read(c->dev->ctx, in->indirect, ind)) return got;
    for (uint32_t i = 0; i < LFS_PPB; i++) {
        if (!lvl[i]) return got;
        if (claim_block(c, lvl[i], ino)) return got;
        got++;
    }
    if (!in->double_indirect) return got;
    if (claim_block(c, in->double_indirect, ino)) return got;
    if (c->dev->read(c->dev->ctx, in->double_indirect, ind)) return got;   /* ind = L1 now */
    for (uint32_t k = 0; k < LFS_PPB; k++) {
        uint32_t sib = lvl[k];
        if (!sib) return got;
        if (claim_block(c, sib, ino)) return got;
        if (c->dev->read(c->dev->ctx, sib, l2)) return got;
        const uint32_t *q = (const uint32_t *)(const void *)l2;
        for (uint32_t m = 0; m < LFS_PPB; m++) {
            if (!q[m]) return got;
            if (claim_block(c, q[m], ino)) return got;
            got++;
        }
    }
    (void)want;
    return got;
}

/* Map file block index -> device block, reading through the chain. */
static uint32_t imap_of(struct ck *c, struct lfs_dinode *in, uint32_t i, uint8_t *tmp)
{
    if (i < LFS_NDIRECT) return in->direct[i];
    i -= LFS_NDIRECT;
    if (i < LFS_PPB) {
        if (!in->indirect || c->dev->read(c->dev->ctx, in->indirect, tmp)) return 0;
        return ((const uint32_t *)(const void *)tmp)[i];
    }
    i -= LFS_PPB;
    if (i < LFS_PPB * LFS_PPB) {
        if (!in->double_indirect ||
            c->dev->read(c->dev->ctx, in->double_indirect, tmp)) return 0;
        uint32_t sib = ((const uint32_t *)(const void *)tmp)[i / LFS_PPB];
        if (!sib || c->dev->read(c->dev->ctx, sib, tmp)) return 0;
        return ((const uint32_t *)(const void *)tmp)[i % LFS_PPB];
    }
    return 0;
}

/* Rewrite one directory block after a repair removed an entry. */
static int put_dirblock(struct ck *c, uint32_t blk, const void *buf)
{
    if (!c->repair || !c->dev->write) return 0;
    return c->dev->write(c->dev->ctx, blk, buf);
}

/* Walk the directory tree from the root, breadth-first with an explicit queue
 * (a recursive walk would blow a kernel stack on a deep tree, and a deep tree is
 * exactly what a corrupt image can present).
 *
 * Two structural findings live here and nowhere else:
 *   - a directory reached a SECOND time is a loop or a second parent. Either
 *     way the back-edge is the wrong one, and removing it is safe and is the
 *     only way to make the tree walkable again.
 *   - a dirent naming a free or out-of-range inode is a dangling name; removing
 *     it loses nothing that still exists. */
static void walk_tree(struct ck *c, uint8_t *dblk, uint8_t *tmp)
{
    uint32_t *queue = kmalloc((size_t)c->sb.inode_count * sizeof(uint32_t));
    if (!queue) return;
    int qh = 0, qt = 0;
    queue[qt++] = c->sb.root_ino;
    c->visited[c->sb.root_ino] = 1;

    while (qh < qt) {
        uint32_t dino = queue[qh++];
        struct lfs_dinode *d = &c->inodes[dino];
        if (d->type != LFS_T_DIR) continue;
        if (d->size % LFS_DIRENT_SZ) {
            c->r->dirent_bad_size++;
            note(c, "directory inode %u: size %u is not a whole number of dirents",
                 dino, d->size);
            if (c->repair) {
                d->size -= d->size % LFS_DIRENT_SZ;
                c->inodes_dirty = 1;
                c->r->fixed++;
            }
        }
        uint32_t nblk = (d->size + LFS_BS - 1) / LFS_BS;
        for (uint32_t bi = 0; bi < nblk; bi++) {
            uint32_t blk = imap_of(c, d, bi, tmp);
            if (!blk || c->dev->read(c->dev->ctx, blk, dblk)) break;
            struct lfs_dirent *de = (struct lfs_dirent *)(void *)dblk;
            int touched = 0;
            for (uint32_t j = 0; j < LFS_BS / LFS_DIRENT_SZ; j++) {
                if (bi * LFS_BS + j * LFS_DIRENT_SZ >= d->size) break;
                if (!de[j].name[0]) continue;
                uint32_t cino = de[j].ino;
                int drop = 0;
                if (cino >= c->sb.inode_count) {
                    c->r->dirent_bad_ino++;
                    note(c, "directory inode %u: entry points at inode %u, out of range",
                         dino, cino);
                    drop = 1;
                } else if (c->inodes[cino].type == LFS_T_FREE) {
                    c->r->dirent_bad_ino++;
                    note(c, "directory inode %u: entry points at FREE inode %u", dino, cino);
                    drop = 1;
                } else if (c->visited[cino] && c->inodes[cino].type == LFS_T_DIR) {
                    c->r->dir_loop++;
                    note(c, "directory inode %u: entry re-enters directory inode %u "
                            "(loop or second parent)", dino, cino);
                    drop = 1;
                } else if (c->nlink[cino]) {
                    c->r->multi_linked++;
                    note(c, "inode %u is named by more than one directory entry "
                            "(seen again from inode %u)", cino, dino);
                    drop = 1;
                }
                if (drop) {
                    if (c->repair) {
                        de[j].ino = 0;
                        de[j].name[0] = 0;
                        touched = 1;
                        c->r->fixed++;
                    }
                    continue;
                }
                if (c->nlink[cino] < 255) c->nlink[cino]++;
                if (c->inodes[cino].type == LFS_T_DIR && !c->visited[cino]) {
                    c->visited[cino] = 1;
                    if (qt < (int)c->sb.inode_count) queue[qt++] = cino;
                }
            }
            if (touched) put_dirblock(c, blk, dblk);
        }
    }
    kfree(queue);
}

int fsck_run(struct fsck_dev *dev, int repair, struct fsck_report *rep,
             fsck_say say, void *saycx)
{
    struct fsck_report local;
    struct ck c;
    uint8_t *sblk = NULL, *ind = NULL, *l2 = NULL, *dblk = NULL, *tmp = NULL;
    int rc = -1;

    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);
    memset(&c, 0, sizeof c);
    c.dev = dev; c.r = rep; c.say = say; c.saycx = saycx; c.repair = repair;
    if (!dev || !dev->read) { rep->fatal = 1; return -1; }
    if (repair && !dev->write) { rep->fatal = 1; return -1; }

    /* A repair pass looks first. If any block is claimed by two inodes, nothing
     * is written at all: "give it to A" and "give it to B" are equally
     * defensible and one of them destroys a file, so there is no repair to make
     * -- only a guess. Probing up front rather than bailing out halfway is what
     * makes the refusal complete: a half-applied repair is exactly the "silently
     * made it worse" outcome a checker must never produce.
     * (The probe is read-only, so it cannot recurse.) */
    if (repair) {
        struct fsck_report probe;
        struct fsck_dev ro = *dev;
        ro.write = NULL;
        fsck_run(&ro, 0, &probe, NULL, NULL);
        if (probe.dup_block) {
            *rep = probe;
            if (say) say(saycx, "%u block(s) claimed by two inodes -- REFUSING to repair: "
                                "nothing on the disk says which claim is the right one",
                         (uint32_t)probe.dup_block, 0);
            return -1;
        }
    }

    sblk = kmalloc(LFS_BS);
    ind  = kmalloc(LFS_BS);
    l2   = kmalloc(LFS_BS);
    dblk = kmalloc(LFS_BS);
    tmp  = kmalloc(LFS_BS);
    if (!sblk || !ind || !l2 || !dblk || !tmp) { rep->fatal = 1; goto out; }

    /* --- 1. superblock ---------------------------------------------------- */
    if (dev->read(dev->ctx, 0, sblk)) { rep->fatal = 1; goto out; }
    memcpy(&c.sb, sblk, sizeof c.sb);
    if (fsck_super_valid(&c.sb)) {
        rep->fatal = 1;
        rep->problems++;
        if (say) say(saycx, "superblock is not a usable LogitFS image: "
                            "magic %u version %u -- refusing", c.sb.magic, c.sb.version);
        goto out;
    }
    if (dev->nblocks && c.sb.total_blocks > dev->nblocks) {
        rep->fatal = 1;
        rep->problems++;
        if (say) say(saycx, "superblock claims %u blocks but the device holds %u "
                            "-- refusing", c.sb.total_blocks, dev->nblocks);
        goto out;
    }

    /* --- 2. the journal, before anything else ----------------------------- */
    /* Checking a filesystem with an outstanding committed transaction would
     * flag every difference the transaction is about to erase. Replay first,
     * exactly as a mount would, then check the result. */
    {
        int discarded = 0;
        struct fsck_dev jdev = *dev;
        if (!repair) jdev.write = NULL;              /* read-only: report only */
        int n = fsck_log_recover(&jdev, &c.sb, &discarded);
        if (n < 0) {
            rep->problems++;
            rep->journal_discarded++;
            if (say) say(saycx, "log header names a target we refuse to write "
                                "-- the log was ignored", 0, 0);
        } else if (n > 0) {
            /* Not a problem: an outstanding committed transaction is the NORMAL
             * state after a crash, and finishing it is what mount does too. It
             * is reported because a reader wants to know it happened. */
            rep->journal_replayed = n;
            if (say) say(saycx, "log: %u committed block(s) from an interrupted "
                                "transaction", (uint32_t)n, 0);
        } else if (discarded) {
            rep->journal_discarded++;
            rep->problems++;
            if (repair) rep->fixed++;
            if (say) say(saycx, "log: header present but not a valid commit record "
                                "(torn or stale) -- discarded", 0, 0);
        }
    }

    /* --- 3. load the bitmap and the inode table --------------------------- */
    c.bitmap = kmalloc((size_t)c.sb.bitmap_blocks * LFS_BS);
    c.inodes = kmalloc((size_t)c.sb.inode_blocks * LFS_BS);
    c.seen   = kmalloc((size_t)((c.sb.total_blocks + 7) / 8));
    c.nlink  = kmalloc(c.sb.inode_count);
    c.visited= kmalloc(c.sb.inode_count);
    if (!c.bitmap || !c.inodes || !c.seen || !c.nlink || !c.visited) { rep->fatal = 1; goto out; }
    memset(c.seen, 0, (c.sb.total_blocks + 7) / 8);
    memset(c.nlink, 0, c.sb.inode_count);
    memset(c.visited, 0, c.sb.inode_count);
    for (uint32_t i = 0; i < c.sb.bitmap_blocks; i++)
        if (dev->read(dev->ctx, c.sb.bitmap_start + i, c.bitmap + (size_t)i * LFS_BS)) { rep->fatal = 1; goto out; }
    for (uint32_t i = 0; i < c.sb.inode_blocks; i++)
        if (dev->read(dev->ctx, c.sb.inode_start + i, (uint8_t *)c.inodes + (size_t)i * LFS_BS)) { rep->fatal = 1; goto out; }

    if (c.inodes[c.sb.root_ino].type != LFS_T_DIR) {
        rep->fatal = 1;
        rep->problems++;
        if (say) say(saycx, "root inode %u is not a directory (type %u) -- refusing",
                     c.sb.root_ino, c.inodes[c.sb.root_ino].type);
        goto out;
    }

    /* --- 4. inodes: types, sizes, block chains ---------------------------- */
    /* Every metadata block is in use by definition. Marking them here means the
     * leak/missing comparison below is a straight diff over the whole image. */
    for (uint32_t b = 0; b < c.sb.data_start && b < c.sb.total_blocks; b++) bset(c.seen, b);

    for (uint32_t ino = 0; ino < c.sb.inode_count; ino++) {
        struct lfs_dinode *in = &c.inodes[ino];
        if (in->type == LFS_T_FREE) continue;
        if (in->type != LFS_T_FILE && in->type != LFS_T_DIR) {
            rep->bad_inode_type++;
            note(&c, "inode %u: unknown type %u", ino, in->type);
            if (repair) { memset(in, 0, sizeof *in); c.inodes_dirty = 1; rep->fixed++; }
            continue;
        }
        /* `size` is a u32 and LFS_MAX_FILE_SZ exceeds 2^32, so the "too big"
         * case is unrepresentable rather than merely unlikely -- the reachable
         * failure is a size whose blocks are not all mapped, checked below. */
        uint32_t want = (in->size + LFS_BS - 1) / LFS_BS;
        uint32_t got  = walk_inode(&c, ino, ind, l2);
        if (got < want) {
            rep->bad_size++;
            note(&c, "inode %u: size needs %u blocks but fewer are mapped", ino, want);
            if (repair) {
                /* Truncate to what is actually reachable. Shrinking can only
                 * ever drop bytes the filesystem could not have returned
                 * anyway, so it has one correct answer.
                 *
                 * The pointers past that point must go too, not just the size:
                 * a bad pointer left behind is found again by the next pass,
                 * and a checker whose repairs do not converge is not a repair. */
                in->size = got * LFS_BS;
                if (got <= LFS_NDIRECT) {
                    for (uint32_t i = got; i < LFS_NDIRECT; i++) in->direct[i] = 0;
                    in->indirect = 0;
                    in->double_indirect = 0;
                } else if (got <= LFS_NDIRECT + LFS_PPB) {
                    in->double_indirect = 0;
                    if (in->indirect && !dev->read(dev->ctx, in->indirect, ind)) {
                        uint32_t *p = (uint32_t *)(void *)ind;
                        for (uint32_t i = got - LFS_NDIRECT; i < LFS_PPB; i++) p[i] = 0;
                        if (dev->write(dev->ctx, in->indirect, ind)) goto out;
                    }
                } else {
                    /* Inside the double-indirect tree: drop the L1 entries from
                     * the failing one on, and trim the L2 it stopped in. */
                    uint32_t j = got - LFS_NDIRECT - LFS_PPB;
                    if (in->double_indirect && !dev->read(dev->ctx, in->double_indirect, ind)) {
                        uint32_t *l1 = (uint32_t *)(void *)ind;
                        uint32_t k = j / LFS_PPB, off = j % LFS_PPB;
                        if (off && l1[k] && !dev->read(dev->ctx, l1[k], l2)) {
                            uint32_t *p = (uint32_t *)(void *)l2;
                            for (uint32_t i = off; i < LFS_PPB; i++) p[i] = 0;
                            if (dev->write(dev->ctx, l1[k], l2)) goto out;
                            k++;
                        }
                        for (uint32_t i = k; i < LFS_PPB; i++) l1[i] = 0;
                        if (dev->write(dev->ctx, in->double_indirect, ind)) goto out;
                    }
                }
                c.inodes_dirty = 1;
                rep->fixed++;
            }
        }
    }

    /* --- 5. directory tree, dangling names, loops, link counts ------------ */
    walk_tree(&c, dblk, tmp);

    for (uint32_t ino = 0; ino < c.sb.inode_count; ino++) {
        if (ino == c.sb.root_ino) continue;
        if (c.inodes[ino].type == LFS_T_FREE) continue;
        if (c.nlink[ino]) continue;
        rep->orphan_inode++;
        note(&c, "inode %u is allocated (type %u) but no directory entry names it",
             ino, c.inodes[ino].type);
        if (repair) {
            /* Nothing can reach it, so freeing it cannot lose a reachable file.
             * Its blocks fall out of `seen` only if we re-walk, so drop them
             * from `seen` explicitly -- otherwise they would stay marked used
             * and become a leak the very next pass reports. */
            memset(&c.inodes[ino], 0, sizeof c.inodes[ino]);
            c.inodes_dirty = 1;
            rep->fixed++;
        }
    }
    if (repair && rep->orphan_inode) {
        /* Re-derive `seen` from the surviving inodes: the cheapest way to keep
         * the bitmap comparison below honest after inodes were freed. */
        memset(c.seen, 0, (c.sb.total_blocks + 7) / 8);
        for (uint32_t b = 0; b < c.sb.data_start && b < c.sb.total_blocks; b++) bset(c.seen, b);
        for (uint32_t ino = 0; ino < c.sb.inode_count; ino++)
            if (c.inodes[ino].type != LFS_T_FREE) (void)walk_inode(&c, ino, ind, l2);
    }

    /* --- 6. bitmap vs. what the inodes actually reference ----------------- */
    /* This is the check the CLAUDE.md corruption note was about, and the two
     * directions are not equally bad. A block referenced but marked FREE will
     * be handed to a second file and silently shared -- that is data loss, and
     * setting the bit is always right. A block marked used but referenced by
     * nothing is only wasted space; clearing the bit is also always right, but
     * getting it wrong costs nothing, so the conservative order is: fix the
     * missing bits first, and count them separately in the report. */
    for (uint32_t b = 0; b < c.sb.total_blocks; b++) {
        int used = bget(c.seen, b), marked = bget(c.bitmap, b);
        if (used && !marked) {
            rep->bitmap_missing++;
            note(&c, "block %u is referenced by an inode but marked FREE", b, 0);
            if (repair) { bset(c.bitmap, b); c.bitmap_dirty = 1; rep->fixed++; }
        } else if (!used && marked) {
            rep->bitmap_leaked++;
            note(&c, "block %u is marked used but nothing references it", b, 0);
            if (repair) { bclr(c.bitmap, b); c.bitmap_dirty = 1; rep->fixed++; }
        }
    }

    /* --- 7. write back --------------------------------------------------- */
    if (repair && c.inodes_dirty)
        for (uint32_t i = 0; i < c.sb.inode_blocks; i++)
            if (dev->write(dev->ctx, c.sb.inode_start + i,
                           (uint8_t *)c.inodes + (size_t)i * LFS_BS)) goto out;
    if (repair && c.bitmap_dirty)
        for (uint32_t i = 0; i < c.sb.bitmap_blocks; i++)
            if (dev->write(dev->ctx, c.sb.bitmap_start + i,
                           c.bitmap + (size_t)i * LFS_BS)) goto out;

    /* A check pass is clean iff it found nothing. A repair pass is clean iff it
     * got this far: the only class it declines to touch is dup_block, and that
     * one returned above without writing a byte. Counting `fixed == problems`
     * instead would be wrong, because one repair can settle two findings (a bad
     * block pointer and the size that depended on it are the same truncation). */
    rc = repair ? 0 : (rep->problems == 0 ? 0 : -1);
out:
    kfree(sblk); kfree(ind); kfree(l2); kfree(dblk); kfree(tmp);
    kfree(c.bitmap); kfree(c.inodes); kfree(c.seen); kfree(c.nlink); kfree(c.visited);
    return rc;
}
