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
};
void bcache_getstats(struct bcache_stats *out);

#endif /* LOGIT_BCACHE_H */
