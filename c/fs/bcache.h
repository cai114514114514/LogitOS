#ifndef LOGIT_BCACHE_H
#define LOGIT_BCACHE_H

#include <stdint.h>

/* The block buffer cache: a fixed pool of 4 KiB buffers between LogitFS and
 * blkdev.
 *
 * Before this existed every bread() was a device round trip. That is worse than
 * it sounds, because LogitFS re-reads the same handful of blocks constantly:
 * imap() reads the indirect block on EVERY block lookup, so reading an n-block
 * file past direct[12] costs n indirect reads of the same block; dir_lookup,
 * dir_nth, dir_count_live and dir_is_empty each re-walk a directory's data
 * blocks from the device; and resolve() runs dir_lookup once per path
 * component, so a four-deep path re-reads four directories on every single
 * syscall.
 *
 * --- The writeback policy, and why it is safe ------------------------------
 *
 * Every write is DEFERRED: bcache_write() updates the buffer, marks it dirty,
 * and returns without touching the device. Dirty buffers reach the device in
 * exactly two ways, and it matters that they are the only two:
 *
 *   1. bcache_sync()  -- writes every dirty buffer, then issues a blk_flush()
 *                        barrier. This is the filesystem's ordering point.
 *   2. eviction       -- the LRU victim is written out if dirty, with no
 *                        barrier, at an arbitrary moment.
 *
 * So the rule the filesystem must obey is:
 *
 *     A DIRTY BUFFER MAY REACH THE DEVICE AT ANY TIME AND IN ANY ORDER
 *     RELATIVE TO ANY OTHER DIRTY BUFFER.
 *
 * Correctness never comes from a write being WITHHELD -- only from a barrier
 * having already happened. That is the whole discipline, and it is what makes
 * the cache safe to put underneath a journal: the journal's argument (see
 * c/fs/logitfs.c) is a case analysis over "what is on media", and an early
 * writeback only ever moves a block onto media SOONER, which is a state the
 * analysis already covers. A cache that tried to help by holding writes back
 * would instead invent orderings the journal never asked for.
 *
 * The one thing a cache must not do is answer a read with stale data, so every
 * write goes through the cache, and the cache is the only writer. bcache_drop()
 * exists for the two callers that legitimately have a different view of the
 * device than the cache does: fsck (which rewrites blocks underneath) and the
 * host crash simulator (which models losing volatile state).
 *
 * Not thread-safe: like the rest of LogitFS it runs under the kernel BKL. */

#define BC_BS       4096          /* must equal logitfs BS */
#define BC_NBUF     256           /* 1 MiB of buffers */

/* Bring the cache up over a device of `total_blocks` blocks with `nbuf`
 * buffers (0 = BC_NBUF). Idempotent-unsafe: call bcache_shutdown() first. */
int  bcache_init(uint32_t total_blocks, int nbuf);
void bcache_shutdown(void);

/* Read a block: from a resident buffer, else from the device into one. */
int  bcache_read(uint32_t blk, void *buf);

/* Read `n` CONSECUTIVE blocks starting at `blk` into `buf` (n * BC_BS bytes).
 *
 * WHY THIS IS NOT A LOOP OVER bcache_read
 * ---------------------------------------
 * Two separate costs, both measured on this machine (make bench-fs):
 *
 *  1. A device round trip costs about 80 us no matter how big it is -- 81 us
 *     for 8 sectors, 120 us for 255. A block-at-a-time loop over a 3 MB file
 *     issues 741 of them and spends 51 ms; coalescing the misses into 512 KiB
 *     commands issues six.
 *
 *  2. The cache was not merely failing to help that read, it was being DESTROYED
 *     by it. 741 blocks streamed through 256 buffers evict every buffer in the
 *     pool including all the metadata a subsequent lookup needs, and none of the
 *     741 is ever read again. The measurement is unambiguous: before this
 *     existed, a WARM read of browser.aex (54.4 ms) was no faster than a COLD
 *     one (51.5 ms), while a warm read of the 12 KB clock.aex was 14x faster
 *     than cold. The cache was helping small files and being ruined by big ones.
 *
 * So a run read serves resident blocks from their buffers, coalesces every
 * contiguous miss into ONE device command straight into the caller's buffer,
 * and installs what it read only when the whole request is small enough not to
 * be a stream (n <= nbuf/4). A big sequential read stays out of the pool.
 *
 * SAFE because the cache is the only writer and a dirty buffer is therefore
 * always resident: any block this function does not find resident has its
 * current contents on the device, which is precisely the block it reads. */
int  bcache_read_run(uint32_t blk, uint32_t n, void *buf);

/* Write a block: resident buffer updated + marked dirty. Does NOT reach the
 * device until an eviction or a bcache_sync(). */
int  bcache_write(uint32_t blk, const void *buf);

/* Write every dirty buffer, then issue a device barrier. Returns -1 if any
 * write or the barrier failed -- and leaves the buffers that failed dirty, so
 * a later sync retries them rather than silently dropping the data. */
int  bcache_sync(void);

/* Discard every buffer, dirty or not, WITHOUT writing anything back. For fsck
 * (which edits the device behind the cache) and the crash simulator. */
void bcache_drop(void);

struct bcache_stats {
    unsigned long hits, misses, evictions, dirty_evictions, writebacks, syncs;
    unsigned long resident, dirty;
    /* What the run reader did. `dev_reads` is the number of device commands the
     * cache issued; `dev_blocks` the blocks they carried. dev_blocks/dev_reads
     * is the coalescing factor, and it is the number that says whether a
     * sequential read is still one round trip per block. */
    unsigned long dev_reads, dev_blocks, bypassed;
};
void bcache_getstats(struct bcache_stats *out);

#endif /* LOGIT_BCACHE_H */
