#include <stdint.h>
#include <stddef.h>
#include "logitfs.h"
#include "logitfs_fmt.h"
#include "bcache.h"
#include "fsck.h"
#include "crc32.h"
#include "blkdev.h"
#include "rtc.h"
#include "kheap.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);   /* lib/string.c */
void *memset(void *, int, size_t);

/* --- on-disk layout: c/fs/logitfs_fmt.h, mirrored by tools/mkfs.py ---
 * Short local aliases for the LFS_* names, because this file mentions them on
 * nearly every line and `NDIRECT` reads better than `LFS_NDIRECT` in an
 * expression that already says which filesystem it is about. */
#define SECTOR     LFS_SECTOR
#define BS         LFS_BS
#define SPB        LFS_SPB
#define MAGIC      LFS_MAGIC
#define VERSION    LFS_VERSION
#define INODE_SIZE LFS_INODE_SIZE
#define NDIRECT    LFS_NDIRECT
#define PPB        LFS_PPB
#define DIRENT_SZ  LFS_DIRENT_SZ
#define NAME_MAX   LFS_NAME_MAX
#define MAX_FILE_SZ LFS_MAX_FILE_SZ
#define T_FREE     LFS_T_FREE
#define T_FILE     LFS_T_FILE
#define T_DIR      LFS_T_DIR
#define MAX_PATH   128
#define NOINO      0xFFFFFFFFu

#define dinode lfs_dinode
#define dirent lfs_dirent

static struct lfs_super sb;

static uint8_t      *bitmap;            /* bitmap_blocks * BS, in RAM */
static struct dinode *inodes;           /* inode_blocks  * BS, in RAM */

/* --- write-ahead log -------------------------------------------------------
 * Every metadata write (bitmap, inode table, indirect pointer blocks, and
 * directory data blocks -- a dirent can point at a fresh inode, so it is
 * metadata too) goes through the log: staged in RAM, written to the log area,
 * and only then installed at its real location.
 *
 * Ordinary file DATA blocks are written straight to their real location, but
 * always BEFORE the metadata that points at them commits (ext4's data=ordered).
 * That is only sound because a block freed by this same operation cannot be
 * reissued to it -- see bfree() below, which is where the in-place overwrite
 * hazard is dealt with.
 *
 * One transaction per VFS mutating op (tx_begin .. log_commit/log_abort). The
 * BKL serializes ops, so there is no nesting. The log is written once per op,
 * not once per block: flush_* below only STAGE blocks.
 *
 * The invariant this gives, the case analysis that establishes it, and the
 * reason the commit record carries checksums are written out in full above
 * log_commit(). Read that before changing anything here. */
#define LOG_MAX    LFS_LOG_MAX          /* cap on sb.log_blocks - 1 */
static uint32_t  log_max;               /* sb.log_blocks - 1 (data slots) */
static uint32_t  tx_targets[LOG_MAX];
static uint8_t (*tx_bufs)[BS];          /* log_max staged blocks, kmalloc'd */
static int       tx_count;
static uint32_t  tx_gen;
static uint8_t   tx_hdr[BS];            /* log header staging */

static uint8_t  blk_buf[BS];            /* general block staging */
static uint32_t ind_buf[PPB];           /* indirect-block staging */
static uint32_t dind_buf[PPB];          /* double-indirect L1 staging */
static uint32_t l2_buf[PPB];            /* double-indirect L2 staging (inode_write) */
static char     namebuf[NAME_MAX];      /* ent_name return storage */

/* Concurrency: every op runs under the kernel BKL and the shared static staging
 * buffers above (blk_buf/ind_buf/dind_buf/namebuf) carry no lock of their own.
 * Correctness relies on the BKL never being dropped mid-operation. */

/* --- helpers --- */
/* Compare a fixed-size on-disk dirent name against a C string without ever
 * reading past NAME_MAX bytes of the on-disk field (a forged image may omit
 * the NUL terminator). */
static int streq(const char *a, const char *b)
{
    for (int i = 0; i < NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}

/* Block numbers come from the disk (inode tables, indirect blocks, superblock)
 * and are untrusted: refuse anything outside the mounted image. */
static int bread(uint32_t blk, void *buf)
{
    if (blk >= sb.total_blocks) return -1;
    /* Read-your-writes inside a transaction: a staged (not yet installed) block
     * must read back fresh -- a same-directory rename re-reads the directory it
     * has just rewritten, and the on-disk copy is still the old one. The buffer
     * cache cannot serve these, on purpose: a staged block is not part of the
     * filesystem state until the transaction commits, and putting it in the
     * cache would make an ABORTED transaction visible to the next reader. */
    for (int i = 0; i < tx_count; i++)
        if (tx_targets[i] == blk) { memcpy(buf, tx_bufs[i], BS); return 0; }
    return bcache_read(blk, buf);
}
/* Deferred: the block is in the cache and dirty, and may reach the device at any
 * moment or not until the next bcache_sync(). Every ordering this filesystem
 * relies on is a bcache_sync(), never the absence of a write. */
static int bwrite(uint32_t blk, const void *buf)
{
    if (blk >= sb.total_blocks) return -1;
    return bcache_write(blk, buf);
}

/* Stage a metadata block into the current transaction (COPIES buf, so callers
 * may keep using their staging buffers). -1 if the log is full: the op fails
 * without committing anything, which is safe -- an over-full log is not. */
static int log_add(uint32_t blk, const void *buf)
{
    if (blk >= sb.total_blocks || (uint32_t)tx_count >= log_max) return -1;
    tx_targets[tx_count] = blk;
    memcpy(tx_bufs[tx_count], buf, BS);
    tx_count++;
    return 0;
}

static void bfree_discard(void);                /* deferred block frees, below */

static void tx_begin(void)  { tx_count = 0; bfree_discard(); }
/* Nothing staged ever reached disk -- and the frees this transaction intended
 * are dropped with it, so the in-RAM bitmap goes on agreeing with the untouched
 * on-disk one. (Before frees were deferred, an aborted operation left the two
 * disagreeing about every block the failed truncate had released.) */
static void log_abort(void) { tx_count = 0; bfree_discard(); }

/* ===========================================================================
 * THE CRASH-CONSISTENCY INVARIANT, and the argument for it
 * ===========================================================================
 *
 * INVARIANT.  At every instant the on-disk image is the state after some prefix
 * T1..Tk of the committed transactions, and nothing else. A crash at any point
 * followed by a mount therefore yields a filesystem in which
 *
 *   (a) every file is byte-for-byte its content before Tk+1 or after Tk+1,
 *       never a mixture;
 *   (b) the free-block bitmap agrees with the inode table -- no block is both
 *       referenced and marked free, and none is referenced by two inodes;
 *   (c) every directory entry names an allocated inode of a valid type.
 *
 * WHY IT HOLDS.  The cut point is the durability of the COMMIT RECORD (the log
 * header). Every crash falls into one of these cases:
 *
 *   1. before the record is on media  -> recovery finds no valid record and
 *      does nothing. The state is the pre-Tk+1 state: all of Tk+1's metadata
 *      existed only in RAM (bitmap/inode table/staged blocks), and the data
 *      blocks it wrote in place are, by the pre-Tk+1 bitmap, free and
 *      unreferenced. B1 is what makes "not on media" reliable in the other
 *      direction: it puts the bodies and this op's data=ordered data writes on
 *      media BEFORE the record, so a record that IS present is never vouching
 *      for bodies that are not.
 *   2. after the record is on media, during the checkpoint -> recovery finds
 *      the record, re-installs all n blocks from the log, and reaches the
 *      post-Tk+1 state. Installing is idempotent, so a half-done checkpoint is
 *      simply finished. B2 is what makes this safe: without it a checkpoint
 *      write could land while the record did not, and recovery would then
 *      discard a transaction that is already half applied.
 *   3. after the checkpoint, before/while the header is cleared -> the record
 *      may still be there; replay redoes installs that already happened.
 *      Idempotent again, so post-Tk+1. B3 keeps the clear from landing before
 *      the checkpoint, which would make a committed transaction vanish.
 *   4. after the clear -> nothing to do, post-Tk+1.
 *
 * WHY THE RECORD CARRIES CHECKSUMS.  Case 1 above needs "no valid record" to
 * really mean "Tk+1 never committed", and two things can break that:
 *
 *   - a TORN header. A 4 KiB block write is not atomic on real hardware. Half a
 *     header can retain a plausible magic and count while the targets are
 *     garbage. `hcrc`, a CRC-32 over the header block, rejects it. This is the
 *     truncated-final-record case every journal has to handle.
 *
 *   - a STALE header standing over newer bodies. This one was a live bug. The
 *     header is cleared at the end of a commit, but that clear is an ordinary
 *     device write with no barrier after it, and the next transaction's first
 *     barrier (B1) comes AFTER it has already written its bodies. A device
 *     write cache is free to land the new bodies and not the clear. Recovery
 *     would then read transaction N's header -- correct magic, correct count,
 *     correct hcrc -- over transaction N+1's body blocks, and write N+1's data
 *     to N's target block numbers. That is corruption CAUSED BY recovery, on a
 *     filesystem that never crashed mid-transaction. `bcrc`, a CRC-32 over
 *     exactly the n body blocks the header describes, makes the combination
 *     unrepresentable. Cost: one CRC pass over <= 63 blocks per commit, and no
 *     extra barrier. (tests/unit/fs_crash_test.c reproduces the bug against the
 *     unchecksummed record and shows it gone with it.)
 *
 * WHY A BUFFER CACHE UNDERNEATH IS SAFE.  bwrite() is now DEFERRED: the block
 * sits dirty in the cache and reaches the device at an unpredictable moment, or
 * at the next bcache_sync(). Every case above is phrased as "what is on media",
 * and an early writeback only ever moves a block onto media SOONER than the
 * barrier would have. Sooner is always a state the analysis already covers:
 * bodies before the record (case 1) and checkpoint blocks after it (case 2/3)
 * are exactly what the barriers were establishing anyway. What the cache must
 * never do is make a block reach media LATER than a barrier claims -- and it
 * cannot, because bcache_sync() writes every dirty buffer before it flushes.
 *
 * ALTERNATIVE CONSIDERED. Ordered writes plus barriers, with no log, is smaller.
 * It was rejected: it needs a defensible order for every mutation (bitmap
 * before inode before dirent, and the reverse for delete), which is a separate
 * argument per operation and gives a WEAKER guarantee -- it can only promise
 * "no dangling reference", not (a). LogitFS rewrites a whole file per write, so
 * the amount of metadata per operation is small and a log is cheap.
 *
 * A barrier failure is recorded in rc but does not abort: it leaves the
 * transaction no worse off than not having called this at all, and rc reaches
 * the caller.
 * ======================================================================== */
static int log_commit(void)
{
    int n = tx_count, rc = 0;
    tx_count = 0;
    if (!n) return 0;

    uint32_t bcrc = CRC32_INIT;
    for (int i = 0; i < n; i++) {
        if (bwrite(sb.log_start + 1 + (uint32_t)i, tx_bufs[i])) rc = -1;
        bcrc = crc32_update(bcrc, tx_bufs[i], BS);
    }
    if (bcache_sync()) rc = -1;                            /* B1 */

    uint32_t *h = (uint32_t *)(void *)tx_hdr;
    memset(tx_hdr, 0, BS);
    h[LFS_LOGH_MAGIC] = LFS_LOG_MAGIC;
    h[LFS_LOGH_GEN]   = ++tx_gen;
    h[LFS_LOGH_COUNT] = (uint32_t)n;
    h[LFS_LOGH_BCRC]  = crc32_final(bcrc);
    for (int i = 0; i < n; i++) h[LFS_LOGH_TARGET + i] = tx_targets[i];
    h[LFS_LOGH_HCRC]  = lfs_log_hdr_crc(tx_hdr);
    if (bwrite(sb.log_start, tx_hdr)) rc = -1;
    if (bcache_sync()) rc = -1;                            /* B2 -- the commit point */

    for (int i = 0; i < n; i++)
        if (bwrite(tx_targets[i], tx_bufs[i])) rc = -1;
    if (bcache_sync()) rc = -1;                            /* B3 */

    /* Clearing the header is only an optimization -- it saves the next mount a
     * pointless (idempotent) replay. It is deliberately NOT barriered: if it is
     * lost, replay redoes an install that already happened, and if it is lost
     * past the NEXT transaction's body writes, bcrc rejects the stale record. */
    memset(tx_hdr, 0, BS);
    if (bwrite(sb.log_start, tx_hdr)) rc = -1;
    return rc;
}

/* --- device shim for fsck.c, which owns replay -----------------------------
 * Recovery lives in fsck.c so that a replay is the same code whether it happens
 * at mount, inside a check, or on the build host over an image file. A bug in
 * one of three copies of the replay rules is precisely the bug nobody finds. */
static int dev_read(void *cx, uint32_t blk, void *buf)        { (void)cx; return bcache_read(blk, buf); }
static int dev_write(void *cx, uint32_t blk, const void *buf) { (void)cx; return bcache_write(blk, buf); }
static int dev_sync(void *cx)                                 { (void)cx; return bcache_sync(); }

static void fsck_dev_init(struct fsck_dev *d, int writable)
{
    d->read  = dev_read;
    d->write = writable ? dev_write : NULL;
    d->sync  = dev_sync;
    d->ctx   = NULL;
    d->nblocks = sb.total_blocks;
}

static int log_recover(void)
{
    struct fsck_dev d;
    int discarded = 0;
    fsck_dev_init(&d, 1);
    int n = fsck_log_recover(&d, &sb, &discarded);
    if (n < 0) {
        kprintf("[fs] log: header names a target outside the image -- ignored\n");
        return 0;              /* refuse the log, keep the filesystem mountable */
    }
    if (n > 0)
        kprintf("[fs] log: replayed %d block(s) from an interrupted transaction\n", n);
    else if (discarded)
        kprintf("[fs] log: an incomplete transaction was discarded (torn or stale record)\n");
    if (n > 0 || discarded) {
        /* The replay wrote through the cache; get it on media before anything
         * else runs, and drop the cache so the tables read below come from the
         * post-recovery image rather than from pre-recovery buffers. */
        if (bcache_sync()) return -1;
        bcache_drop();
    }
    return 0;
}

/* --- timestamps ------------------------------------------------------------
 * Whole seconds from the RTC, in the inode's formerly-reserved bytes (see
 * logitfs_fmt.h for why this is not a format bump). Zero means "never set",
 * which is what a pre-timestamp image reads back as -- callers must not read it
 * as 1970.
 *
 * atime is LAZY on purpose. Updating it the way mtime is updated would mean a
 * full journal transaction -- three barriers -- for every read() of every file,
 * which would make reading a directory more expensive than writing one. So
 * inode_read() stamps atime in the in-RAM inode table only; it reaches the disk
 * whenever that inode block is next written for some other reason, and is lost
 * on a crash. That is the same trade Linux calls `lazytime`, and it is the only
 * version of atime worth having on a filesystem that journals every metadata
 * write synchronously. */
#define TS_A 1
#define TS_M 2
#define TS_C 4

static int64_t (*ts_now)(void) = rtc_unix;   /* indirection: tests pin the clock */

void logitfs_set_clock(int64_t (*fn)(void)) { ts_now = fn ? fn : rtc_unix; }

static void itouch(struct dinode *in, int which)
{
    if (!in) return;
    int64_t t = ts_now ? ts_now() : 0;
    if (which & TS_A) in->atime = t;
    if (which & TS_M) in->mtime = t;
    if (which & TS_C) in->ctime = t;
}

static int  bit_test(uint32_t b)  { return bitmap[b >> 3] & (1 << (b & 7)); }
static void bit_set(uint32_t b)   { bitmap[b >> 3] |=  (1 << (b & 7)); }
static void bit_clear(uint32_t b) { bitmap[b >> 3] &= ~(1 << (b & 7)); }

static uint32_t balloc(void)
{
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++)
        if (!bit_test(b)) { bit_set(b); return b; }
    return 0;                            /* disk full (0 = none) */
}

/* --- freeing is DEFERRED to commit, and that is load-bearing ---------------
 *
 * Freeing a block immediately let balloc hand it straight back within the same
 * transaction, and an overwrite does exactly that: inode_write truncates the
 * file (freeing block B) and then allocates for the new content, gets B again,
 * and writes the new bytes into B. But file data is data=ordered -- written
 * IN PLACE, before the metadata commits. A crash there rolls the metadata back
 * to the old inode, which still points at B... and B now holds a mixture of old
 * and new bytes. The file is neither its old self nor its new self, which is
 * precisely the guarantee the journal is supposed to give.
 *
 * (Found by tests/unit/fs_crash_test.c's `overwrite` sweep. The QEMU crash
 * harness never saw it because it only ever creates a NEW file, which has no
 * old blocks to reuse.)
 *
 * So bfree() only records the intent: the bit stays set in `bitmap`, so balloc
 * cannot reissue the block, and flush_bitmap() -- which every operation calls
 * immediately before it commits -- applies the frees for real. An aborted
 * transaction throws the intent away instead, leaving the in-RAM bitmap
 * agreeing with the untouched disk.
 *
 * bfree_now() is the other case: undoing an allocation THIS transaction made
 * and never told anyone about. Those blocks did not exist before the operation,
 * so releasing them at once is right, and deferring them would leak. */
static uint8_t  *freed;                  /* bit per block, pending release */
static uint32_t  nfreed;

static void bfree(uint32_t b)
{
    if (!b || b >= sb.total_blocks || !freed) return;
    if (!(freed[b >> 3] & (1 << (b & 7)))) nfreed++;
    freed[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static void bfree_now(uint32_t b) { if (b && b < sb.total_blocks) bit_clear(b); }

/* Apply every deferred free. Called from flush_bitmap, i.e. once per operation,
 * immediately before the bitmap is staged into the log. */
static void bfree_apply(void)
{
    if (!nfreed || !freed) return;
    for (uint32_t b = 0; b < sb.total_blocks; b++)
        if (freed[b >> 3] & (1 << (b & 7))) bit_clear(b);
    memset(freed, 0, (size_t)((sb.total_blocks + 7) / 8));
    nfreed = 0;
}

static void bfree_discard(void)
{
    if (!nfreed || !freed) return;
    memset(freed, 0, (size_t)((sb.total_blocks + 7) / 8));
    nfreed = 0;
}

/* An on-disk size is untrusted, and MAX_FILE_SZ is the wrong bound for it:
 * imap() can address ~4.3 GiB, which is more than a uint32_t can hold, so
 * `size > MAX_FILE_SZ` was a comparison that could never be true. The bound
 * that actually bites is the IMAGE. A size claiming more blocks than the disk
 * has is impossible by construction, and refusing it is what stops a forged
 * inode turning a directory scan into a million-block walk with the BKL held. */
static int size_ok(uint32_t sz) { return (uint64_t)sz <= (uint64_t)sb.total_blocks * BS; }

static struct dinode *iget(uint32_t ino)
{
    return (ino < sb.inode_count) ? &inodes[ino] : NULL;
}

static int ialloc(uint16_t type)
{
    for (uint32_t i = 0; i < sb.inode_count; i++)
        if (inodes[i].type == T_FREE) {
            memset(&inodes[i], 0, sizeof(struct dinode));
            inodes[i].type = type;
            itouch(&inodes[i], TS_A | TS_M | TS_C);
            return (int)i;
        }
    return -1;
}

/* block number of a file's i-th data block (clobbers ind_buf), or 0 */
static uint32_t imap(struct dinode *in, uint32_t i)
{
    if (i < NDIRECT) return in->direct[i];
    i -= NDIRECT;
    if (i < PPB) {                                  /* single indirect */
        if (!in->indirect) return 0;
        if (bread(in->indirect, ind_buf)) return 0;
        return ind_buf[i];
    }
    i -= PPB;
    if (i < PPB * PPB) {                             /* double indirect */
        if (!in->double_indirect) return 0;
        if (bread(in->double_indirect, ind_buf)) return 0;
        uint32_t ib = ind_buf[i / PPB];
        if (!ib || bread(ib, ind_buf)) return 0;
        return ind_buf[i % PPB];
    }
    return 0;
}

static int flush_bitmap(void)           /* stage the free-block bitmap */
{
    /* Every operation calls this immediately before it commits, which makes it
     * the one place where "this transaction is going through" is known -- so it
     * is where the deferred frees become real. */
    bfree_apply();
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
        if (log_add(sb.bitmap_start + i, bitmap + i * BS)) return -1;
    return 0;
}

/* stage only the inode-table block that holds inode `ino` */
static int flush_inode(uint32_t ino)
{
    uint32_t blk = ino / (BS / INODE_SIZE);
    return log_add(sb.inode_start + blk, (uint8_t *)inodes + blk * BS);
}

/* --- whole-file I/O --- */
static int inode_read(struct dinode *in, void *buf, int max)
{
    uint32_t size = in->size;
    /* size is untrusted on-disk data: compare unsigned so a value >= 2^31
     * can't wrap negative and bypass the bound, and cap it to what imap() can
     * actually reach (keeps nblk below from wrapping too). */
    if (max < 0 || size > (uint32_t)max || !size_ok(size)) return -1;
    uint8_t  *out  = buf;
    uint32_t  nblk = (size + BS - 1) / BS;
    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t blk = imap(in, i);                  /* direct / single / double indirect */
        if (!blk) return -1;
        uint32_t off = i * BS;
        uint32_t n   = size - off < BS ? size - off : BS;
        if (n == BS) { if (bread(blk, out + off)) return -1; }
        else { if (bread(blk, blk_buf)) return -1; memcpy(out + off, blk_buf, n); }
    }
    itouch(in, TS_A);      /* lazytime: RAM only, no transaction (see above) */
    return (int)size;
}

/* `rel` is how blocks are released: bfree (deferred to commit -- the normal
 * case, and what stops an overwrite reusing its own blocks) or bfree_now (this
 * transaction is being abandoned and these blocks are its own). */
static void inode_trunc_ex(struct dinode *in, void (*rel)(uint32_t))
{
    uint32_t nblk = (in->size + BS - 1) / BS;
    for (uint32_t i = 0; i < nblk && i < NDIRECT; i++) { rel(in->direct[i]); in->direct[i] = 0; }
    if (in->indirect) {
        if (!bread(in->indirect, ind_buf))
            for (uint32_t i = NDIRECT; i < nblk && i < NDIRECT + PPB; i++) rel(ind_buf[i - NDIRECT]);
        rel(in->indirect);
        in->indirect = 0;
    }
    if (in->double_indirect) {                 /* free the whole 2-level tree (files > ~4 MiB) */
        if (!bread(in->double_indirect, dind_buf))
            for (uint32_t x = 0; x < PPB; x++)
                if (dind_buf[x]) {
                    if (!bread(dind_buf[x], ind_buf))
                        for (uint32_t y = 0; y < PPB; y++) if (ind_buf[y]) rel(ind_buf[y]);
                    rel(dind_buf[x]);
                }
        rel(in->double_indirect);
        in->double_indirect = 0;
    }
    in->size = 0;
}

static void inode_trunc(struct dinode *in)     { inode_trunc_ex(in, bfree); }
static void inode_trunc_now(struct dinode *in) { inode_trunc_ex(in, bfree_now); }

/* logged=1: data blocks are staged into the transaction too (directories --
 * a dirent can point at an inode created in the same op, so the whole thing
 * must commit or roll back as one). logged=0: data blocks are written straight
 * to disk, but always before the metadata commit (data=ordered).
 * Pointer blocks (single/double indirect) are metadata and ALWAYS logged. */
static int inode_write(struct dinode *in, const void *buf, int size, int logged)
{
    if (size < 0) return -1;
    uint32_t nblk = ((uint32_t)size + BS - 1) / BS;
    if ((uint64_t)nblk > (uint64_t)NDIRECT + PPB + (uint64_t)PPB * PPB) return -1;
    if ((uint64_t)nblk + 2 > sb.total_blocks - sb.data_start) return -1;   /* cannot fit this disk */

    struct dinode saved_inode = *in;            /* restored verbatim on failure */
    inode_trunc(in);                            /* frees are DEFERRED: see bfree */
    const uint8_t *src = buf;

    /* Track EVERY balloc, not just the ones already linked: a block linked only
     * into a staged (not yet logged) pointer block is invisible to inode_trunc,
     * so the failure path cannot rely on the tree alone. */
    uint32_t cap = nblk + PPB + 2;              /* data + L2s + indirect + L1 */
    uint32_t *allocated = kmalloc(cap * sizeof(uint32_t));
    if (!allocated) return -1;
    int nalloc = 0;
    uint32_t ind = 0, dind = 0, l2 = 0;

    if (nblk > NDIRECT) {
        ind = balloc();
        if (!ind) goto fail;
        allocated[nalloc++] = ind;
        memset(ind_buf, 0, BS);
    }
    if (nblk > NDIRECT + PPB) {
        dind = balloc();
        if (!dind) goto fail;
        allocated[nalloc++] = dind;
        memset(dind_buf, 0, BS);
    }

    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t blk = balloc();
        if (!blk) goto fail;
        allocated[nalloc++] = blk;
        uint32_t off = i * BS;
        uint32_t n   = (uint32_t)size - off < BS ? (uint32_t)size - off : BS;
        int     wrc;
        if (n == BS) wrc = logged ? log_add(blk, src + off) : bwrite(blk, src + off);
        else {
            memset(blk_buf, 0, BS); memcpy(blk_buf, src + off, n);
            wrc = logged ? log_add(blk, blk_buf) : bwrite(blk, blk_buf);
        }
        if (wrc) goto fail;
        if (i < NDIRECT) in->direct[i] = blk;
        else if (i < NDIRECT + PPB) ind_buf[i - NDIRECT] = blk;
        else {
            uint32_t j = i - NDIRECT - PPB;
            if (j % PPB == 0) {                 /* seal the previous L2, open a new one */
                if (l2) {
                    dind_buf[j / PPB - 1] = l2;
                    if (log_add(l2, l2_buf)) goto fail;
                }
                l2 = balloc();
                if (!l2) goto fail;
                allocated[nalloc++] = l2;
                memset(l2_buf, 0, BS);
            }
            l2_buf[j % PPB] = blk;
        }
    }
    if (l2) {                                   /* seal the last L2 */
        uint32_t last = ((uint32_t)nblk - NDIRECT - PPB + PPB - 1) / PPB - 1;
        dind_buf[last] = l2;
        if (log_add(l2, l2_buf)) goto fail;
    }
    if (ind)  { in->indirect = ind;   if (log_add(ind, ind_buf)) goto fail; }
    if (dind) { in->double_indirect = dind; if (log_add(dind, dind_buf)) goto fail; }
    in->size = (uint32_t)size;
    /* The one place every content change goes through -- files via
     * logitfs_write, directories via dir_add/dir_remove -- so a directory's
     * mtime moves when an entry is created or removed, as it should. */
    itouch(in, TS_M | TS_C);
    kfree(allocated);
    return size;
fail:
    /* These blocks were allocated by THIS call and nothing has been told about
     * them, so they go back immediately -- deferring them would leak them for
     * an operation that is about to be abandoned. */
    for (int j = 0; j < nalloc; j++) bfree_now(allocated[j]);
    kfree(allocated);
    /* Put the inode back exactly as it was. The old content is still on disk
     * (nothing committed) and its blocks are still marked used (inode_trunc's
     * frees are deferred and the abort will discard them), so restoring the
     * pointers restores a file that is genuinely whole -- which is better than
     * the previous behaviour of zeroing the inode and losing it. */
    *in = saved_inode;
    return -1;
}

/* --- directory ops --- */
static uint32_t dir_lookup(struct dinode *d, const char *name)
{
    uint32_t sz = d->size;
    if (!size_ok(sz)) return 0;              /* forged size: refuse a BKL-held mega-scan */
    uint32_t nblk = (sz + BS - 1) / BS;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) break;   /* sparse/corrupt chain: stop scanning */
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0] && streq(de[j].name, name)) return de[j].ino;
        }
    }
    return 0;                            /* not found (root ino 0 is never a child) */
}

/* enumerate: return ino of the idx-th live entry of dir `dino`, fill nameout */
static uint32_t dir_nth(uint32_t dino, int idx, char *nameout)
{
    struct dinode *d = iget(dino);
    if (!d || d->type != T_DIR) return 0;
    uint32_t sz = d->size;
    if (!size_ok(sz)) return 0;
    uint32_t nblk = (sz + BS - 1) / BS;
    int seen = 0;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) break;
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0] == 0 || de[j].ino == 0) continue;   /* ino 0 (root) is never a child */
            if (seen == idx) {
                if (nameout) {
                    int k = 0;
                    while (de[j].name[k] && k < NAME_MAX - 1) { nameout[k] = de[j].name[k]; k++; }
                    nameout[k] = 0;
                }
                return de[j].ino;
            }
            seen++;
        }
    }
    return 0;
}

/* Count live entries in one pass (calling dir_nth per index was O(n^2)
 * re-scanning under the BKL -- a system-wide stall on a large directory). */
static int dir_count_live(uint32_t dino)
{
    struct dinode *d = iget(dino);
    if (!d || d->type != T_DIR) return 0;
    uint32_t sz = d->size;
    if (!size_ok(sz)) return 0;
    uint32_t nblk = (sz + BS - 1) / BS;
    int n = 0;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) break;
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0] && de[j].ino) n++;
        }
    }
    return n;
}

static int dir_is_empty(struct dinode *d)
{
    uint32_t sz = d->size;
    if (!size_ok(sz)) return 0;              /* treat corrupt dirs as non-empty: don't rmdir them */
    uint32_t nblk = (sz + BS - 1) / BS;
    for (uint32_t bi = 0; bi < nblk; bi++) {
        uint32_t blk = imap(d, bi);
        if (!blk || bread(blk, blk_buf)) break;
        struct dirent *de = (struct dirent *)blk_buf;
        for (int j = 0; j < BS / DIRENT_SZ; j++) {
            if (bi * BS + (uint32_t)j * DIRENT_SZ >= sz) break;
            if (de[j].name[0]) return 0;
        }
    }
    return 1;
}

static int dir_add(uint32_t dino, const char *name, uint32_t child)
{
    struct dinode *d = iget(dino);
    uint32_t old = d->size;
    /* old is on-disk data: bound it before old + DIRENT_SZ can wrap the
     * allocation size (and before the (int)old cast below can go negative). */
    if (!size_ok(old) || old > 0xFFFFFFFFu - DIRENT_SZ) return -1;
    uint32_t cap = old + DIRENT_SZ;
    uint8_t *buf = kmalloc(cap);
    if (!buf) return -1;
    memset(buf, 0, cap);
    if (old && inode_read(d, buf, (int)old) < 0) { kfree(buf); return -1; }

    struct dirent *de = (struct dirent *)buf;
    int n = (int)(old / DIRENT_SZ), slot = -1;
    for (int i = 0; i < n; i++) if (de[i].name[0] == 0) { slot = i; break; }
    uint32_t newsize = old;
    if (slot < 0) { slot = n; newsize = old + DIRENT_SZ; }

    de[slot].ino = child;
    int k = 0;
    while (name[k] && k < NAME_MAX - 1) { de[slot].name[k] = name[k]; k++; }
    de[slot].name[k] = 0;

    int rc = inode_write(d, buf, (int)newsize, 1);   /* directory: fully logged */
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

static int dir_remove(uint32_t dino, const char *name)
{
    struct dinode *d = iget(dino);
    uint32_t sz = d->size;
    if (!sz || !size_ok(sz)) return -1;
    uint8_t *buf = kmalloc(sz);
    if (!buf) return -1;
    if (inode_read(d, buf, (int)sz) < 0) { kfree(buf); return -1; }

    struct dirent *de = (struct dirent *)buf;
    int n = (int)(sz / DIRENT_SZ), found = -1;
    for (int i = 0; i < n; i++) if (de[i].name[0] && streq(de[i].name, name)) { found = i; break; }
    if (found < 0) { kfree(buf); return -1; }
    de[found].name[0] = 0;
    de[found].ino = 0;

    int rc = inode_write(d, buf, (int)sz, 1);        /* directory: fully logged */
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

/* --- path resolution --- */
static uint32_t resolve(const char *path)
{
    uint32_t stack[MAX_PATH / 2];
    int sp = 0;
    stack[sp++] = sb.root_ino;
    uint32_t cur = sb.root_ino;
    const char *p = path;
    char comp[NAME_MAX];

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != '/' && k < NAME_MAX - 1) comp[k++] = *p++;
        comp[k] = 0;
        if (*p && *p != '/') return NOINO;    /* over-long component: refuse, don't match a truncated prefix */

        if (comp[0] == '.' && comp[1] == 0) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (sp > 1) sp--;
            cur = stack[sp - 1];
            continue;
        }
        struct dinode *d = iget(cur);
        if (!d || d->type != T_DIR) return NOINO;
        uint32_t child = dir_lookup(d, comp);
        if (!child) return NOINO;
        cur = child;
        if (sp < (int)(sizeof stack / sizeof stack[0])) stack[sp++] = cur;
    }
    return cur;
}

/* split path into (parent dir ino, leaf name); NOINO on error */
static uint32_t resolve_parent(const char *path, char *leaf)
{
    int len = 0; while (path[len]) len++;
    int e = len; while (e > 0 && path[e - 1] == '/') e--;
    int s = e;   while (s > 0 && path[s - 1] != '/') s--;

    int k = 0;
    for (int i = s; i < e && k < NAME_MAX - 1; i++) leaf[k++] = path[i];
    leaf[k] = 0;
    if (k == 0 || e - s > NAME_MAX - 1) return NOINO;   /* empty or over-long leaf */
    if (leaf[0] == '.' && (leaf[1] == 0 || (leaf[1] == '.' && leaf[2] == 0)))
        return NOINO;                                   /* never create/remove "." or ".." entries */

    char dirpath[MAX_PATH];
    int dl = 0;
    for (int i = 0; i < s && dl < MAX_PATH - 1; i++) dirpath[dl++] = path[i];
    dirpath[dl] = 0;
    if (dl == 0) { dirpath[0] = '/'; dirpath[1] = 0; }
    return resolve(dirpath);
}

/* --- VFS ops --- */
/* Render one fsck finding. Both numeric arguments are always passed; the
 * message carries at most two %u (see fsck.h). */
static void fsck_report_line(void *cx, const char *msg, uint32_t a, uint32_t b)
{
    (void)cx;
    kprintf("[fsck] ");
    kprintf(msg, a, b);
    kprintf("\n");
}

/* Check the mounted filesystem. repair != 0 writes the fixes back.
 * Returns 0 if clean (or fully repaired). Safe to call only when no transaction
 * is open, which under the BKL means "not from inside a VFS op". */
int logitfs_fsck(int repair)
{
    if (!bitmap || !inodes) return -1;
    struct fsck_dev d;
    struct fsck_report r;
    fsck_dev_init(&d, repair);
    /* fsck reads the tables off the device; flush anything dirty first, and
     * drop the cache afterwards because a repair rewrote blocks underneath. */
    if (bcache_sync()) return -1;
    int rc = fsck_run(&d, repair, &r, fsck_report_line, NULL);
    if (repair) {
        bcache_sync();
        bcache_drop();
        /* The in-RAM tables are now stale relative to what fsck wrote. */
        for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
            if (bread(sb.bitmap_start + i, bitmap + i * BS)) return -1;
        for (uint32_t i = 0; i < sb.inode_blocks; i++)
            if (bread(sb.inode_start + i, (uint8_t *)inodes + i * BS)) return -1;
    }
    if (r.fatal)
        kprintf("[fsck] REFUSED: the image is not repairable in place\n");
    else if (!r.problems)
        kprintf("[fsck] clean\n");
    else
        kprintf("[fsck] %d problem(s), %d repaired (dup-blocks %d, bitmap %d/%d, "
                "dirents %d, loops %d, orphans %d)\n",
                r.problems, r.fixed, r.dup_block, r.bitmap_missing, r.bitmap_leaked,
                r.dirent_bad_ino, r.dir_loop, r.orphan_inode);
    return rc;
}

/* Everything dirty in the cache to media, plus a barrier. There is no
 * per-file variant to add: a mutating VFS op is one transaction and commits
 * before it returns, so a completed vfs_write is ALREADY durable and this is
 * the whole-device `sync(2)`. It exists for the caller that wants to be sure
 * after a burst of activity, and for shutdown. */
int logitfs_sync(void)
{
    if (!bitmap || !inodes) return -1;
    return bcache_sync();
}

static int logitfs_mount(void)
{
    if (bitmap || inodes) return -1;           /* already mounted: no re-entry */
    uint8_t b0[SECTOR];
    if (blk_read(0, 1, b0)) return -1;
    memcpy(&sb, b0, sizeof sb);
    /* Geometry validation lives in fsck.c, shared with the offline checker: the
     * mounted and the checked notion of "usable image" must not drift apart,
     * and every one of those bounds exists because the value it bounds comes
     * off the disk and then sizes or indexes an allocation. */
    if (fsck_super_valid(&sb)) return -1;

    log_max = sb.log_blocks - 1;
    if (bcache_init(sb.total_blocks, 0)) return -1;
    bitmap = kmalloc((size_t)((uint64_t)sb.bitmap_blocks * BS));
    inodes = kmalloc((size_t)((uint64_t)sb.inode_blocks  * BS));
    tx_bufs = kmalloc((size_t)log_max * BS);
    freed   = kmalloc((size_t)((sb.total_blocks + 7) / 8));
    if (!bitmap || !inodes || !tx_bufs || !freed) goto oom;
    memset(freed, 0, (size_t)((sb.total_blocks + 7) / 8));
    nfreed = 0;
    tx_count = 0;
    if (log_recover()) goto oom;           /* finish an interrupted transaction, if any */
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
        if (bread(sb.bitmap_start + i, bitmap + i * BS)) goto oom;
    for (uint32_t i = 0; i < sb.inode_blocks; i++)
        if (bread(sb.inode_start + i, (uint8_t *)inodes + i * BS)) goto oom;

    /* A READ-ONLY consistency check on every mount.
     *
     * It repairs nothing -- a repair pass rewrites the inode table and the
     * bitmap, and a filesystem that quietly rewrites itself at boot is a much
     * worse thing to debug than one that tells you it is damaged. What it buys
     * is that every boot in this tree, including all the harnesses that were
     * written for something else entirely, now asserts that the bitmap agrees
     * with the inodes, that no block is claimed twice, and that the directory
     * tree is a tree. For a 64 MiB image with 256 inodes that is a few hundred
     * mostly-cached block reads.
     *
     * A failure here never fails the mount: the filesystem is still the best
     * one available, and refusing to boot over a leaked block would be a worse
     * trade than saying so and carrying on. */
    {
        struct fsck_dev d;
        struct fsck_report r;
        fsck_dev_init(&d, 0);
        fsck_run(&d, 0, &r, NULL, NULL);
        if (r.fatal)
            kprintf("[fsck] mount check: the image is structurally unusable\n");
        else if (r.problems)
            kprintf("[fsck] mount check: %d problem(s) -- dup-block %d, bitmap %d/%d, "
                    "dirent %d, loop %d, orphan %d, size %d, ptr %d\n",
                    r.problems, r.dup_block, r.bitmap_missing, r.bitmap_leaked,
                    r.dirent_bad_ino, r.dir_loop, r.orphan_inode, r.bad_size, r.bad_blockptr);
        else
            kprintf("[fsck] mount check: clean\n");
    }
    return 0;
oom:
    kfree(bitmap); kfree(inodes); kfree(tx_bufs); kfree(freed);
    bitmap = 0; inodes = 0; tx_bufs = 0; freed = 0; nfreed = 0;
    bcache_shutdown();
    return -1;
}

/* Drop the mount. bcache_shutdown() writes back anything still dirty, so this
 * is the CLEAN unmount; a caller simulating power loss calls bcache_drop()
 * first. Needed by the crash tests, which mount the same image dozens of times
 * in one process, and by any future eject/shutdown path. */
void logitfs_unmount(void)
{
    if (!bitmap && !inodes) return;
    bcache_shutdown();
    kfree(bitmap); kfree(inodes); kfree(tx_bufs); kfree(freed);
    bitmap = 0; inodes = 0; tx_bufs = 0; freed = 0; nfreed = 0;
    tx_count = 0;
    tx_gen = 0;
}

static int logitfs_size(const char *path)
{
    uint32_t ino = resolve(path);
    if (ino == NOINO) return -1;
    struct dinode *in = iget(ino);
    /* -1 for directories (callers open/size files) and for on-disk sizes that
     * don't fit the int return value. */
    if (!in || in->type != T_FILE || in->size > (uint32_t)INT32_MAX) return -1;
    return (int)in->size;
}

static int logitfs_read(const char *path, void *buf, int max)
{
    uint32_t ino = resolve(path);
    if (ino == NOINO) return -1;
    struct dinode *in = iget(ino);
    if (!in || in->type != T_FILE) return -1;
    return inode_read(in, buf, max);
}

static int logitfs_write(const char *path, const void *buf, int size)
{
    uint32_t ino = resolve(path);
    if (ino != NOINO) {                          /* overwrite existing file */
        struct dinode *in = iget(ino);
        if (!in || in->type != T_FILE) return -1;
        tx_begin();
        if (inode_write(in, buf, size, 0) < 0) { log_abort(); return -1; }
        if (flush_inode(ino) || flush_bitmap()) { log_abort(); return -1; }
        return log_commit() ? -1 : size;
    }
    char leaf[NAME_MAX];                          /* else create */
    uint32_t parent = resolve_parent(path, leaf);
    if (parent == NOINO) return -1;
    struct dinode *pd = iget(parent);
    if (!pd || pd->type != T_DIR) return -1;
    int ni = ialloc(T_FILE);
    if (ni < 0) return -1;
    struct dinode *in = iget((uint32_t)ni);
    tx_begin();
    if (inode_write(in, buf, size, 0) < 0) { log_abort(); in->type = T_FREE; return -1; }
    /* The inode and its blocks were created inside this transaction, so they go
     * back immediately (inode_trunc_now); log_abort discards the deferred frees
     * that dir_add's own failed rewrite may have queued for the DIRECTORY,
     * whose old blocks are still live. */
    if (dir_add(parent, leaf, (uint32_t)ni) < 0) { log_abort(); inode_trunc_now(in); in->type = T_FREE; return -1; }
    if (flush_inode((uint32_t)ni) || flush_inode(parent) || flush_bitmap()) { log_abort(); return -1; }
    return log_commit() ? -1 : size;
}

static int logitfs_mkdir(const char *path)
{
    if (resolve(path) != NOINO) return -1;        /* already exists */
    char leaf[NAME_MAX];
    uint32_t parent = resolve_parent(path, leaf);
    if (parent == NOINO) return -1;
    struct dinode *pd = iget(parent);
    if (!pd || pd->type != T_DIR) return -1;
    int ni = ialloc(T_DIR);          /* ialloc zeroes the inode (size 0, no blocks) */
    if (ni < 0) return -1;
    struct dinode *nd = iget((uint32_t)ni);
    /* Create an EMPTY directory: "." and ".." are NOT stored on disk -- path
     * resolution (resolve()/proc_resolve) handles . and .. itself, and the Finder
     * draws ".." on its own. Storing them made `ls` list them, made an empty dir
     * look non-empty (couldn't rmdir), and was inconsistent with mkfs-built dirs.
     * The whole op is one transaction: the dirent and the fresh inode commit
     * together or not at all, so no intermediate flush ordering is needed. */
    tx_begin();
    if (dir_add(parent, leaf, (uint32_t)ni) < 0) { log_abort(); nd->type = T_FREE; return -1; }
    if (flush_inode((uint32_t)ni) || flush_inode(parent) || flush_bitmap()) { log_abort(); return -1; }
    return log_commit() ? -1 : 0;
}

static int logitfs_delete(const char *path)
{
    uint32_t ino = resolve(path);
    if (ino == NOINO || ino == sb.root_ino) return -1;
    struct dinode *in = iget(ino);
    if (!in) return -1;
    if (in->type == T_DIR && !dir_is_empty(in)) return -1;
    char leaf[NAME_MAX];
    uint32_t parent = resolve_parent(path, leaf);
    if (parent == NOINO) return -1;
    tx_begin();
    if (dir_remove(parent, leaf) < 0) { log_abort(); return -1; }
    inode_trunc(in);
    in->type = T_FREE;
    if (flush_inode(ino) || flush_inode(parent) || flush_bitmap()) { log_abort(); return -1; }
    return log_commit() ? -1 : 0;
}

/* Directory-scoped enumeration. */
static uint32_t resolve_dir(const char *dir)
{
    uint32_t ino = resolve(dir);
    if (ino == NOINO) return NOINO;
    struct dinode *d = iget(ino);
    return (d && d->type == T_DIR) ? ino : NOINO;
}

static int logitfs_count(const char *dir)
{
    uint32_t ino = resolve_dir(dir);
    return ino == NOINO ? -1 : dir_count_live(ino);   /* -1: not a directory */
}

static const char *logitfs_ent_name(const char *dir, int i)
{
    namebuf[0] = 0;
    uint32_t ino = resolve_dir(dir);
    if (ino != NOINO) dir_nth(ino, i, namebuf);
    return namebuf;
}

static int logitfs_ent_size(const char *dir, int i)
{
    uint32_t dino = resolve_dir(dir);
    if (dino == NOINO) return 0;
    uint32_t ino = dir_nth(dino, i, NULL);
    struct dinode *in = ino ? iget(ino) : NULL;
    return (in && in->size <= (uint32_t)INT32_MAX) ? (int)in->size : 0;
}

static int logitfs_ent_is_dir(const char *dir, int i)
{
    uint32_t dino = resolve_dir(dir);
    if (dino == NOINO) return 0;
    uint32_t ino = dir_nth(dino, i, NULL);
    struct dinode *in = ino ? iget(ino) : NULL;
    return in && in->type == T_DIR;
}

static void logitfs_list(void)
{
    int n = dir_count_live(sb.root_ino);
    kprintf("[fs] LogitFS v4: %d entr(ies) in /:\n", n);
    for (int i = 0; i < n; i++) {
        char nm[NAME_MAX]; nm[0] = 0;
        uint32_t ino = dir_nth(sb.root_ino, i, nm);
        struct dinode *in = iget(ino);
        kprintf("[fs]   %-16s %s %u bytes\n", nm,
                (in && in->type == T_DIR) ? "<dir>" : "     ",
                in ? in->size : 0);
    }
}

/* 1 iff normalized absolute path `b` equals `a` or is nested under it (a/...).
 * Both are kernel-resolved absolute paths; we ignore a single trailing '/'. */
static int path_under(const char *a, const char *b)
{
    int la = 0; while (a[la]) la++;
    while (la > 1 && a[la - 1] == '/') la--;     /* drop trailing slash(es) */
    int lb = 0; while (b[lb]) lb++;
    while (lb > 1 && b[lb - 1] == '/') lb--;
    if (lb < la) return 0;
    for (int i = 0; i < la; i++) if (a[i] != b[i]) return 0;
    return lb == la || b[la] == '/';
}

/* Move/rename: re-link a directory entry. Resolve everything first (the shared
 * static blk_buf/ind_buf make interleaving resolution with dir_add/dir_remove
 * unsafe), then mutate add-then-remove inside ONE transaction -- a crash sees
 * the entry in exactly one place, never both and never neither. */
static int logitfs_rename(const char *old_path, const char *new_path)
{
    uint32_t src = resolve(old_path);
    if (src == NOINO || src == sb.root_ino) return -1;
    if (resolve(new_path) != NOINO) return -1;            /* no clobber */

    char old_leaf[NAME_MAX], new_leaf[NAME_MAX];
    uint32_t op = resolve_parent(old_path, old_leaf);
    if (op == NOINO) return -1;
    uint32_t np = resolve_parent(new_path, new_leaf);
    if (np == NOINO) return -1;
    struct dinode *npd = iget(np);
    if (!npd || npd->type != T_DIR) return -1;

    /* Cycle guard: can't move a directory into itself or a descendant. */
    struct dinode *si = iget(src);
    if (si && si->type == T_DIR && path_under(old_path, new_path)) return -1;

    tx_begin();
    if (dir_add(np, new_leaf, src) < 0) { log_abort(); return -1; }
    if (dir_remove(op, old_leaf) < 0) {
        /* Nothing committed yet: discard the transaction and unwind the add in
         * RAM so the in-memory inode/bitmap agree with the untouched disk. */
        dir_remove(np, new_leaf);
        log_abort();
        return -1;
    }
    itouch(si, TS_C);                       /* the inode's name changed, not its content */
    if (flush_inode(src) || flush_inode(op) || flush_inode(np) || flush_bitmap())
        { log_abort(); return -1; }
    return log_commit() ? -1 : 0;
}

struct filesystem logitfs = {
    .name     = "logitfs",
    .mount    = logitfs_mount,
    .list     = logitfs_list,
    .size     = logitfs_size,
    .read     = logitfs_read,
    .count      = logitfs_count,
    .ent_name   = logitfs_ent_name,
    .ent_size   = logitfs_ent_size,
    .ent_is_dir = logitfs_ent_is_dir,
    .write    = logitfs_write,
    .del      = logitfs_delete,
    .mkdir    = logitfs_mkdir,
    .rename   = logitfs_rename,
};
