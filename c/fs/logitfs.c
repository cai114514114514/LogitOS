#include <stdint.h>
#include <stddef.h>
#include "logitfs.h"
#include "blkdev.h"
#include "kheap.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);   /* lib/string.c */
void *memset(void *, int, size_t);

/* --- on-disk layout (must match tools/mkfs.py) --- */
#define SECTOR     512
#define BS         4096                 /* block size */
#define SPB        (BS / SECTOR)        /* sectors per block (8) */
#define MAGIC      0x4C4F4749u          /* "LOGI" */
#define VERSION    4
#define INODE_SIZE 128
#define NDIRECT    12
#define PPB        (BS / 4)             /* u32 pointers per indirect block */
#define DIRENT_SZ  64
#define NAME_MAX   60
#define MAX_FILE_SZ ((uint64_t)(NDIRECT + PPB + (uint64_t)PPB * PPB) * BS) /* inode_write reach: direct + single + double indirect */
#define T_FREE     0
#define T_FILE     1
#define T_DIR      2
#define MAX_PATH   128
#define NOINO      0xFFFFFFFFu

struct dinode {                         /* 128 bytes on disk */
    uint16_t type;
    uint16_t pad;
    uint32_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;
    uint32_t double_indirect;
    uint8_t  reserved[INODE_SIZE - 8 - NDIRECT * 4 - 8];
} __attribute__((packed));

struct dirent {                         /* 64 bytes on disk */
    uint32_t ino;
    char     name[NAME_MAX];
} __attribute__((packed));

static struct {
    uint32_t magic, version, block_size, total_blocks, inode_count;
    uint32_t bitmap_start, bitmap_blocks, inode_start, inode_blocks;
    uint32_t data_start, root_ino;
    uint32_t log_start, log_blocks;
} sb;

static uint8_t      *bitmap;            /* bitmap_blocks * BS, in RAM */
static struct dinode *inodes;           /* inode_blocks  * BS, in RAM */

/* --- write-ahead log -------------------------------------------------------
 * Every metadata write (bitmap, inode table, indirect pointer blocks, and
 * directory data blocks -- a dirent can point at a fresh inode, so it is
 * metadata too) goes through the log: it is staged in RAM, written to the log
 * area, and only then installed at its real location. A crash anywhere in that
 * sequence is resolved at mount: a complete log header replays the install
 * (idempotent), a missing one discards the transaction whole. So after a crash
 * every file is either fully old or fully new, and the bitmap never disagrees
 * with the inode table.
 *
 * Ordinary file DATA blocks are written straight to their real location, but
 * always BEFORE the metadata that points at them commits (ext4's data=ordered):
 * a crash mid-data-write rolls the metadata back, so the new blocks simply turn
 * free again -- never shared, never leaked.
 *
 * One transaction per VFS mutating op (tx_begin .. log_commit/log_abort). The
 * BKL serializes ops, so there is no nesting. The log is written once per op,
 * not once per block: flush_* below only STAGE blocks. */
#define LOGMAGIC   0x4C4F4734u          /* "LOG4" */
#define LOG_MAX    128                  /* cap on sb.log_blocks - 1 */
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
     * has just rewritten, and the on-disk copy is still the old one. */
    for (int i = 0; i < tx_count; i++)
        if (tx_targets[i] == blk) { memcpy(buf, tx_bufs[i], BS); return 0; }
    return blk_read(blk * SPB, SPB, buf);
}
static int bwrite(uint32_t blk, const void *buf)
{
    if (blk >= sb.total_blocks) return -1;
    return blk_write(blk * SPB, SPB, buf);
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

static void tx_begin(void)  { tx_count = 0; }
static void log_abort(void) { tx_count = 0; }   /* nothing staged ever reached disk */

/* Write the staged blocks to the log, seal it with a valid header, install them
 * at their real locations, then clear the header. A crash before the seal loses
 * the whole transaction; a crash after it replays the install at mount. */
/* Three barriers, because a completed bwrite() means the DEVICE has the block,
 * not the platter -- and a disk reorders freely inside its own write cache.
 * Every step below orders against the next one, and each has a distinct failure
 * if it is missing:
 *
 *   B1  staged log blocks (and, before them, this op's data=ordered data writes)
 *       must be on media before the commit record is. Without it a crash can
 *       leave the header on media vouching for log blocks that never landed --
 *       and recovery then dutifully copies that garbage over good data. This is
 *       the one that makes an unbarriered journal WORSE than no journal.
 *   B2  the commit record must be on media before any checkpoint write is.
 *       Without it a checkpoint can land while the record does not, so recovery
 *       discards a transaction that is already half applied.
 *   B3  the checkpoint must be on media before the header is cleared. Without it
 *       the clear can land first and a committed transaction vanishes entirely.
 *
 * Failures are recorded but do not abort: a barrier that could not be issued
 * leaves the transaction no worse off than it was before this function existed,
 * and rc already propagates to the caller. */
static int log_commit(void)
{
    int n = tx_count, rc = 0;
    tx_count = 0;
    if (!n) return 0;
    for (int i = 0; i < n; i++)
        if (bwrite(sb.log_start + 1 + i, tx_bufs[i])) rc = -1;
    if (blk_flush()) rc = -1;                              /* B1 */
    uint32_t *h = (uint32_t *)tx_hdr;
    memset(tx_hdr, 0, BS);
    h[0] = LOGMAGIC; h[1] = ++tx_gen; h[2] = (uint32_t)n;
    for (int i = 0; i < n; i++) h[3 + i] = tx_targets[i];
    if (bwrite(sb.log_start, tx_hdr)) rc = -1;
    if (blk_flush()) rc = -1;                              /* B2 */
    for (int i = 0; i < n; i++)
        if (bwrite(tx_targets[i], tx_bufs[i])) rc = -1;
    if (blk_flush()) rc = -1;                              /* B3 */
    memset(tx_hdr, 0, BS);
    if (bwrite(sb.log_start, tx_hdr)) rc = -1;
    return rc;
}

/* Mount-time recovery: a valid header means a committed transaction whose
 * install may not have finished. Re-install (idempotent) and clear. A torn or
 * absent header fails the magic/count checks and means "nothing to do" -- the
 * pre-transaction state is intact. (Like xv6, we assume a header block write
 * does not tear in a way that preserves magic + count + a corrupted target.) */
static int log_recover(void)
{
    if (bread(sb.log_start, tx_hdr)) return -1;
    uint32_t *h = (uint32_t *)tx_hdr;
    if (h[0] != LOGMAGIC || h[2] == 0 || h[2] > log_max) return 0;
    int n = (int)h[2];
    tx_gen = h[1];
    for (int i = 0; i < n; i++) {
        uint32_t tgt = h[3 + i];
        /* forged header: refuse the superblock, the log itself, and anything
         * outside the image rather than letting recovery overwrite them */
        if (tgt == 0 || tgt >= sb.total_blocks) return -1;
        if (tgt >= sb.log_start && tgt < sb.log_start + sb.log_blocks) return -1;
        if (bread(sb.log_start + 1 + i, tx_bufs[i])) return -1;
        if (bwrite(tgt, tx_bufs[i])) return -1;
    }
    /* Same reason as B3: if the header clear reaches media before the replayed
     * blocks do, a second crash loses the transaction that recovery just
     * restored -- and the log no longer says how to restore it again. */
    if (blk_flush()) return -1;
    memset(tx_hdr, 0, BS);
    if (bwrite(sb.log_start, tx_hdr)) return -1;
    if (blk_flush()) return -1;
    kprintf("[fs] log: replayed %d block(s) from an interrupted transaction\n", n);
    return 0;
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
static void bfree(uint32_t b) { if (b && b < sb.total_blocks) bit_clear(b); }

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
    if (max < 0 || size > (uint32_t)max || size > MAX_FILE_SZ) return -1;
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
    return (int)size;
}

static void inode_trunc(struct dinode *in)
{
    uint32_t nblk = (in->size + BS - 1) / BS;
    for (uint32_t i = 0; i < nblk && i < NDIRECT; i++) { bfree(in->direct[i]); in->direct[i] = 0; }
    if (in->indirect) {
        if (!bread(in->indirect, ind_buf))
            for (uint32_t i = NDIRECT; i < nblk && i < NDIRECT + PPB; i++) bfree(ind_buf[i - NDIRECT]);
        bfree(in->indirect);
        in->indirect = 0;
    }
    if (in->double_indirect) {                 /* free the whole 2-level tree (files > ~4 MiB) */
        if (!bread(in->double_indirect, dind_buf))
            for (uint32_t x = 0; x < PPB; x++)
                if (dind_buf[x]) {
                    if (!bread(dind_buf[x], ind_buf))
                        for (uint32_t y = 0; y < PPB; y++) if (ind_buf[y]) bfree(ind_buf[y]);
                    bfree(dind_buf[x]);
                }
        bfree(in->double_indirect);
        in->double_indirect = 0;
    }
    in->size = 0;
}

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

    inode_trunc(in);
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
    kfree(allocated);
    return size;
fail:
    for (int j = 0; j < nalloc; j++) bfree(allocated[j]);
    kfree(allocated);
    /* Don't leave dangling block pointers behind: the entry inode_trunc() ran
     * when size was already 0, so it won't clear the direct[]/indirect values
     * set above -- and the staged pointer blocks were never installed. */
    memset(in->direct, 0, sizeof in->direct);
    in->indirect = 0;
    in->double_indirect = 0;
    in->size = 0;
    return -1;
}

/* --- directory ops --- */
static uint32_t dir_lookup(struct dinode *d, const char *name)
{
    uint32_t sz = d->size;
    if (sz > MAX_FILE_SZ) return 0;          /* forged size: refuse a BKL-held mega-scan */
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
    if (sz > MAX_FILE_SZ) return 0;
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
    if (sz > MAX_FILE_SZ) return 0;
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
    if (sz > MAX_FILE_SZ) return 0;          /* treat corrupt dirs as non-empty: don't rmdir them */
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
    if (old > MAX_FILE_SZ - DIRENT_SZ) return -1;
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
    if (!sz || sz > MAX_FILE_SZ) return -1;
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
static int logitfs_mount(void)
{
    if (bitmap || inodes) return -1;           /* already mounted: no re-entry */
    uint8_t b0[SECTOR];
    if (blk_read(0, 1, b0)) return -1;
    uint32_t *w = (uint32_t *)b0;
    if (w[0] != MAGIC || w[1] != VERSION || w[2] != BS) return -1;
    sb.magic = w[0]; sb.version = w[1]; sb.block_size = w[2];
    sb.total_blocks = w[3]; sb.inode_count = w[4];
    sb.bitmap_start = w[5]; sb.bitmap_blocks = w[6];
    sb.inode_start = w[7]; sb.inode_blocks = w[8];
    sb.data_start = w[9]; sb.root_ino = w[10];
    sb.log_start = w[11]; sb.log_blocks = w[12];

    /* Bound the untrusted on-disk counts before sizing: bitmap_blocks*BS is
     * uint32*int and would wrap to a tiny (or zero) allocation for a crafted
     * superblock, then the bread loops below write BS bytes/block past it.
     * Legit images use bitmap_blocks=1, inode_blocks=8. */
    if (sb.bitmap_blocks == 0 || sb.bitmap_blocks > 1024) return -1;   /* >32 MiB bitmap = absurd */
    if (sb.inode_blocks  == 0 || sb.inode_blocks  > 4096) return -1;   /* >16 MiB inodes = absurd */
    /* inode_count bounds iget()/ialloc() indexing, but the inodes[] buffer is
     * sized by inode_blocks -- tie them together or a crafted count walks past
     * the allocation. */
    if (sb.inode_count == 0 || sb.inode_count > (uint32_t)sb.inode_blocks * (BS / INODE_SIZE)) return -1;
    /* total_blocks indexes the free bitmap via balloc()/bit_set(); a crafted
     * value past the bitmap's bit coverage is an OOB heap write on any alloc. */
    if (sb.total_blocks > (uint32_t)sb.bitmap_blocks * BS * 8) return -1;
    /* Region bounds: keep every area inside total_blocks (64-bit sums so a
     * crafted start can't wrap the comparison) and in mkfs order so the areas
     * can't overlap each other or the superblock in block 0. */
    if (sb.bitmap_start == 0) return -1;
    if ((uint64_t)sb.bitmap_start + sb.bitmap_blocks > sb.total_blocks) return -1;
    if ((uint64_t)sb.inode_start + sb.inode_blocks > sb.total_blocks) return -1;
    if (sb.data_start > sb.total_blocks) return -1;
    if (sb.inode_start < sb.bitmap_start + sb.bitmap_blocks) return -1;
    if (sb.data_start < sb.inode_start + sb.inode_blocks) return -1;
    /* The log sits between the inode table and the data area, with at least a
     * header block and one data block, and within the static staging cap. */
    if (sb.log_blocks < 2 || sb.log_blocks - 1 > LOG_MAX) return -1;
    if (sb.log_start < sb.inode_start + sb.inode_blocks) return -1;
    if ((uint64_t)sb.log_start + sb.log_blocks > sb.data_start) return -1;
    if (sb.root_ino >= sb.inode_count) return -1;
    log_max = sb.log_blocks - 1;
    bitmap = kmalloc((size_t)((uint64_t)sb.bitmap_blocks * BS));
    inodes = kmalloc((size_t)((uint64_t)sb.inode_blocks  * BS));
    tx_bufs = kmalloc((size_t)log_max * BS);
    if (!bitmap || !inodes || !tx_bufs) goto oom;
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
        if (bread(sb.bitmap_start + i, bitmap + i * BS)) goto oom;
    for (uint32_t i = 0; i < sb.inode_blocks; i++)
        if (bread(sb.inode_start + i, (uint8_t *)inodes + i * BS)) goto oom;
    tx_count = 0;
    if (log_recover()) goto oom;           /* finish an interrupted transaction, if any */
    return 0;
oom:
    kfree(bitmap); kfree(inodes); kfree(tx_bufs);
    bitmap = 0; inodes = 0; tx_bufs = 0;
    return -1;
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
    if (dir_add(parent, leaf, (uint32_t)ni) < 0) { log_abort(); inode_trunc(in); in->type = T_FREE; return -1; }
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
    if (flush_inode(op) || flush_inode(np) || flush_bitmap()) { log_abort(); return -1; }
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
