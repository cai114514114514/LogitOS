#ifndef LOGIT_FS_SIM_H
#define LOGIT_FS_SIM_H

/* A simulated disk for the LogitFS host tests, and the small mkfs that seeds it.
 *
 * The point of this file is the DEVICE MODEL. A test that writes straight into
 * an array cannot fail for an ordering reason, so it would pass against a
 * journal with no barriers at all -- which is the exact test that lets the bug
 * class this whole line exists to close go unnoticed. So the simulated device
 * behaves the way real ones do:
 *
 *   - a write is accepted into a VOLATILE cache and is NOT on media;
 *   - a read sees the volatile cache (the device is coherent with itself);
 *   - blk_flush() is the only thing that puts the cache on media;
 *   - a power cut applies an ARBITRARY SUBSET of the volatile cache to media,
 *     which covers "everything landed", "nothing landed", and every reordering
 *     in between as the seed varies;
 *   - a power cut may TEAR one in-flight block, writing a prefix of it. Real
 *     4 KiB writes are not atomic, and a torn commit record is what a journal's
 *     record checksum exists to reject.
 *
 * Crash injection is a write budget plus setjmp/longjmp: when the budget runs
 * out mid-operation the device longjmps out of the filesystem, which is what a
 * power cut does to a call stack -- no unwinding, no cleanup, and every byte of
 * RAM state (buffer cache, in-memory bitmap and inode table) discarded.
 *
 * Include this in exactly ONE translation unit per test binary: it defines
 * blk_read/blk_write/blk_flush, which logitfs.c and bcache.c link against. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "logitfs_fmt.h"

#define SIM_MAX_PENDING 8192

int     fsstub_verbose = 0;
int64_t fsstub_clock   = 1750000000;   /* a fixed, obviously-not-1970 instant */

struct sim_pending { uint32_t blk; uint8_t data[LFS_BS]; };

static uint8_t            *sim_media;
static uint32_t            sim_nblocks;
static struct sim_pending *sim_pend;
static int                 sim_npend;

/* Crash injection. budget < 0 means "never crash". */
static long          sim_budget = -1;
static long          sim_writes;          /* writes issued since sim_reset_counters */
static jmp_buf       sim_crash_jmp;
static int           sim_crash_armed;
static unsigned long sim_barriers;
/* Device READ accounting. "The filesystem coalesces" is a claim about how many
 * times the device was asked for data, and the only place that is observable is
 * the device -- so the simulated one counts commands and the blocks they
 * carried, and a test can assert the ratio. */
static unsigned long sim_reads, sim_read_blocks;

static uint32_t sim_rand_state = 1;
static uint32_t sim_rand(void)
{
    sim_rand_state = sim_rand_state * 1103515245u + 12345u;
    return (sim_rand_state >> 16) & 0x7FFFu;
}
static void sim_srand(uint32_t s) { sim_rand_state = s ? s : 1; }

/* --- the device ----------------------------------------------------------- */

static int sim_pend_find(uint32_t blk)
{
    for (int i = sim_npend - 1; i >= 0; i--) if (sim_pend[i].blk == blk) return i;
    return -1;
}

/* Reads are per-SECTOR, because the driver's are: logitfs_mount reads the
 * superblock as one 512-byte sector, everything else works in 8-sector blocks.
 * A sector is served from the volatile cache if the block containing it is
 * pending -- the device is coherent with itself even when media is not. */
int blk_read(uint32_t lba, uint8_t count, void *buf)
{
    uint8_t *out = buf;
    if (!count) return -1;
    if ((uint64_t)lba + count > (uint64_t)sim_nblocks * LFS_SPB) return -1;
    sim_reads++;
    sim_read_blocks += count / LFS_SPB;
    for (uint32_t s = 0; s < count; s++) {
        uint32_t blk = (lba + s) / LFS_SPB, off = ((lba + s) % LFS_SPB) * LFS_SECTOR;
        int p = sim_pend_find(blk);
        const uint8_t *src = (p >= 0) ? sim_pend[p].data
                                      : sim_media + (size_t)blk * LFS_BS;
        memcpy(out + (size_t)s * LFS_SECTOR, src + off, LFS_SECTOR);
    }
    return 0;
}

int blk_read_n(uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *out = (uint8_t *)buf;
    if (!count) return -1;
    if (lba + count > (uint64_t)sim_nblocks * LFS_SPB) return -1;
    sim_reads++;
    sim_read_blocks += count / LFS_SPB;
    for (uint32_t s = 0; s < count; s++) {
        uint32_t blk = (uint32_t)((lba + s) / LFS_SPB), off = (uint32_t)(((lba + s) % LFS_SPB) * LFS_SECTOR);
        int p = sim_pend_find(blk);
        const uint8_t *src = (p >= 0) ? sim_pend[p].data
                                      : sim_media + (size_t)blk * LFS_BS;
        memcpy(out + (size_t)s * LFS_SECTOR, src + off, LFS_SECTOR);
    }
    return 0;
}

int blk_write(uint32_t lba, uint8_t count, const void *buf)
{
    if (count != LFS_SPB || (lba % LFS_SPB)) return -1;
    uint32_t blk = lba / LFS_SPB;
    if (blk >= sim_nblocks) return -1;

    sim_writes++;
    if (sim_budget >= 0 && --sim_budget < 0 && sim_crash_armed) {
        sim_crash_armed = 0;
        longjmp(sim_crash_jmp, 1);           /* the plug comes out, mid-write */
    }
    int p = sim_pend_find(blk);
    if (p < 0) {
        if (sim_npend >= SIM_MAX_PENDING) { fprintf(stderr, "sim: pending overflow\n"); abort(); }
        p = sim_npend++;
        sim_pend[p].blk = blk;
    }
    memcpy(sim_pend[p].data, buf, LFS_BS);
    return 0;
}

int blk_flush(void)
{
    sim_barriers++;
    for (int i = 0; i < sim_npend; i++)
        memcpy(sim_media + (size_t)sim_pend[i].blk * LFS_BS, sim_pend[i].data, LFS_BS);
    sim_npend = 0;
    return 0;
}

unsigned long blk_flush_count(void) { return sim_barriers; }

/* Power cut: an arbitrary subset of what the device accepted but had not
 * flushed reaches media, and at most one block lands torn. Everything else is
 * simply gone. */
static void sim_power_cut(uint32_t seed, int allow_tear)
{
    sim_srand(seed);
    int tear_at = (allow_tear && sim_npend) ? (int)(sim_rand() % (uint32_t)sim_npend) : -1;
    for (int i = 0; i < sim_npend; i++) {
        if (sim_rand() & 1) continue;                  /* this one never made it */
        uint8_t *dst = sim_media + (size_t)sim_pend[i].blk * LFS_BS;
        if (i == tear_at) {
            /* A prefix of the block landed. 512-byte granularity, because that
             * is the sector size the device actually commits in. */
            uint32_t sectors = sim_rand() % LFS_SPB;   /* 0..7 of 8 */
            memcpy(dst, sim_pend[i].data, (size_t)sectors * LFS_SECTOR);
        } else {
            memcpy(dst, sim_pend[i].data, LFS_BS);
        }
    }
    sim_npend = 0;
}

/* --- image ---------------------------------------------------------------- */

#define SIM_TOTAL_BLOCKS 512      /* default: 2 MiB, enough for direct + single indirect */
#define SIM_INODE_COUNT  64
#define SIM_LOG_BLOCKS   16

/* The same geometry tools/mkfs.py computes, in C, so the tests need no Python
 * and no build/disk.img. tests/unit/fs_format_test.c checks this against a real
 * mkfs image, which is what keeps the two from drifting. */
static void sim_mkfs(void)
{
    memset(sim_media, 0, (size_t)sim_nblocks * LFS_BS);
    uint32_t bitmap_start  = 1;
    uint32_t bitmap_blocks = (sim_nblocks + 8 * LFS_BS - 1) / (8 * LFS_BS);
    uint32_t inode_start   = bitmap_start + bitmap_blocks;
    uint32_t inode_blocks  = (SIM_INODE_COUNT * LFS_INODE_SIZE + LFS_BS - 1) / LFS_BS;
    uint32_t log_start     = inode_start + inode_blocks;
    uint32_t data_start    = log_start + SIM_LOG_BLOCKS;

    uint32_t sbw[13] = { LFS_MAGIC, LFS_VERSION, LFS_BS, sim_nblocks,
                         SIM_INODE_COUNT, bitmap_start, bitmap_blocks,
                         inode_start, inode_blocks, data_start, 0,
                         log_start, SIM_LOG_BLOCKS };
    memcpy(sim_media, sbw, sizeof sbw);

    uint8_t *bm = sim_media + (size_t)bitmap_start * LFS_BS;
    for (uint32_t b = 0; b < data_start; b++) bm[b >> 3] |= (uint8_t)(1u << (b & 7));

    struct lfs_dinode *root =
        (struct lfs_dinode *)(void *)(sim_media + (size_t)inode_start * LFS_BS);
    memset(root, 0, sizeof *root);
    root->type  = LFS_T_DIR;
    root->size  = 0;
    root->atime = root->mtime = root->ctime = fsstub_clock;

    sim_npend = 0;
}

/* `nblocks` = 0 uses SIM_TOTAL_BLOCKS. A test that needs the DOUBLE-INDIRECT
 * tree must pass more than NDIRECT + PPB + overhead blocks (~1100), because
 * nothing below that reaches the second level at all. */
static void sim_open_n(uint32_t nblocks)
{
    sim_nblocks = nblocks ? nblocks : SIM_TOTAL_BLOCKS;
    sim_media = calloc(sim_nblocks, LFS_BS);
    sim_pend  = calloc(SIM_MAX_PENDING, sizeof *sim_pend);
    if (!sim_media || !sim_pend) { fprintf(stderr, "sim: out of memory\n"); abort(); }
    sim_mkfs();
}

static void sim_open(void) { sim_open_n(0); }

static void sim_close(void) { free(sim_media); free(sim_pend); sim_media = 0; sim_pend = 0; }

/* Read the superblock straight off media, bypassing everything. */
static void sim_read_super(struct lfs_super *sb) { memcpy(sb, sim_media, sizeof *sb); }

#endif /* LOGIT_FS_SIM_H */
