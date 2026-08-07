/* The buffer cache: what it caches, what it evicts, and -- the part that
 * matters for durability -- what it is allowed to do with a dirty buffer.
 *
 * The cache's contract (c/fs/bcache.h) is deliberately permissive in one
 * direction and absolute in the other:
 *
 *   PERMISSIVE  a dirty buffer may reach the device at any time, in any order,
 *               without a barrier -- eviction does exactly that.
 *   ABSOLUTE    after bcache_sync() returns, every write issued before it is on
 *               media, and a barrier has been issued.
 *
 * The journal's crash argument is a case analysis over what is on media, and it
 * holds because early writeback only ever moves a block onto media SOONER than
 * a barrier would have. What would break it is a write landing LATER than a
 * barrier claims, so that is the property tested hardest here.
 */

#include "fs_sim.h"
#include "fs_check.h"
#include "bcache.h"

static uint8_t buf[LFS_BS], got[LFS_BS];

static void pattern(uint8_t *b, int tag)
{
    for (int i = 0; i < LFS_BS; i++) b[i] = (uint8_t)(tag * 61 + i * 13 + (i >> 7));
}

/* What is on MEDIA (not what the device would return -- the device is coherent
 * with its own volatile cache, media is not). */
static int on_media(uint32_t blk, int tag)
{
    pattern(buf, tag);
    return memcmp(sim_media + (size_t)blk * LFS_BS, buf, LFS_BS) == 0;
}

/* How many pending (accepted but unflushed) writes the device holds. */
static int pending(void) { return sim_npend; }

int main(void)
{
    struct bcache_stats st;
    sim_open();

    /* --- reads are served from the cache ---------------------------------- */
    fs_ok(bcache_init(sim_nblocks, 8) == 0, "init");
    pattern(buf, 1);
    memcpy(sim_media + (size_t)100 * LFS_BS, buf, LFS_BS);

    fs_ok(bcache_read(100, got) == 0 && memcmp(got, buf, LFS_BS) == 0, "first read is correct");
    bcache_getstats(&st);
    fs_ok(st.misses == 1 && st.hits == 0, "first read is a miss");
    fs_ok(bcache_read(100, got) == 0, "second read succeeds");
    bcache_getstats(&st);
    fs_ok(st.hits == 1, "second read is a hit -- no device round trip");

    /* A read that hits must not reach the device at all: prove it by changing
     * MEDIA underneath and requiring the cache to keep its own answer. */
    memset(sim_media + (size_t)100 * LFS_BS, 0xAB, LFS_BS);
    fs_ok(bcache_read(100, got) == 0 && memcmp(got, buf, LFS_BS) == 0,
          "a cache hit does not re-read the device");

    /* --- writes are deferred ---------------------------------------------- */
    bcache_drop();
    pattern(buf, 2);
    fs_ok(bcache_write(200, buf) == 0, "write accepted");
    fs_ok(!on_media(200, 2), "a write is NOT on media before a sync");
    fs_ok(pending() == 0, "and has not even been handed to the device yet");
    fs_ok(bcache_read(200, got) == 0 && memcmp(got, buf, LFS_BS) == 0,
          "but it reads back -- the cache must never answer stale");

    unsigned long b0 = blk_flush_count();
    fs_ok(bcache_sync() == 0, "sync succeeds");
    fs_ok(on_media(200, 2), "after sync the write IS on media");
    fs_ok(blk_flush_count() == b0 + 1, "sync issues exactly one barrier");
    bcache_getstats(&st);
    fs_ok(st.dirty == 0, "sync leaves nothing dirty");

    /* --- sync is complete, not partial ------------------------------------- */
    /* Every block written before a sync must be on media after it, not just the
     * most recent -- with more dirty blocks than the cache has buffers, so the
     * path exercises eviction as well. */
    bcache_shutdown();
    memset(sim_media, 0, (size_t)sim_nblocks * LFS_BS);
    sim_npend = 0;
    fs_ok(bcache_init(sim_nblocks, 8) == 0, "re-init with 8 buffers");
    for (int i = 0; i < 40; i++) { pattern(buf, 100 + i); bcache_write(200 + (uint32_t)i, buf); }
    bcache_sync();
    int allthere = 1;
    for (int i = 0; i < 40; i++) if (!on_media(200 + (uint32_t)i, 100 + i)) allthere = 0;
    fs_ok(allthere, "40 writes through an 8-buffer cache: every one is on media after sync");

    /* --- eviction ---------------------------------------------------------- */
    /* A clean buffer evicted is invisible. A DIRTY buffer evicted must be
     * written out, never dropped: dropping it is silent data loss, and it is the
     * failure a cache can hide for a long time. */
    bcache_shutdown();
    memset(sim_media, 0, (size_t)sim_nblocks * LFS_BS);
    sim_npend = 0;
    bcache_init(sim_nblocks, 4);
    for (int i = 0; i < 4; i++) { pattern(buf, 300 + i); bcache_write(50 + (uint32_t)i, buf); }
    fs_ok(pending() == 0, "4 writes into 4 buffers: nothing pushed at the device yet");
    pattern(buf, 999);
    bcache_write(60, buf);                       /* forces one eviction */
    bcache_getstats(&st);
    fs_ok(st.dirty_evictions >= 1, "a fifth write evicts a dirty buffer");
    fs_ok(pending() >= 1, "and the evicted buffer was WRITTEN OUT, not discarded");
    bcache_sync();
    int survived = 1;
    for (int i = 0; i < 4; i++) if (!on_media(50 + (uint32_t)i, 300 + i)) survived = 0;
    fs_ok(survived, "every evicted dirty buffer's data survives to media");

    /* LRU: the block touched most recently must be the last one evicted. */
    bcache_shutdown();
    memset(sim_media, 0, (size_t)sim_nblocks * LFS_BS);
    bcache_init(sim_nblocks, 4);
    for (uint32_t i = 0; i < 4; i++) { pattern(buf, (int)i); memcpy(sim_media + (size_t)(10 + i) * LFS_BS, buf, LFS_BS); }
    for (uint32_t i = 0; i < 4; i++) bcache_read(10 + i, got);   /* fill: LRU order 10,11,12,13 */
    bcache_read(10, got);                                        /* 10 becomes most recent */
    bcache_read(20, got);                                        /* evicts the LRU, which is 11 */
    bcache_getstats(&st);
    unsigned long hits_before = st.hits;
    bcache_read(10, got);
    bcache_getstats(&st);
    fs_ok(st.hits == hits_before + 1, "LRU: the recently used block is still resident");
    hits_before = st.hits;
    bcache_read(11, got);
    bcache_getstats(&st);
    fs_ok(st.hits == hits_before, "LRU: the least recently used block is the one evicted");

    /* --- ordering: nothing may land AFTER the barrier it was written before -- */
    /* Written, synced, then a power cut with no further flush. The synced block
     * must be on media; a block written after the sync need not be, and either
     * way must not corrupt the first. */
    bcache_shutdown();
    memset(sim_media, 0, (size_t)sim_nblocks * LFS_BS);
    sim_npend = 0;
    bcache_init(sim_nblocks, 16);
    pattern(buf, 55); bcache_write(300, buf);
    bcache_sync();
    pattern(buf, 66); bcache_write(301, buf);
    sim_power_cut(7, 0);
    bcache_drop();
    fs_ok(on_media(300, 55), "a block written before a sync survives a power cut");

    /* --- drop discards, it does not write back ------------------------------ */
    bcache_shutdown();
    memset(sim_media, 0, (size_t)sim_nblocks * LFS_BS);
    sim_npend = 0;
    bcache_init(sim_nblocks, 16);
    pattern(buf, 77); bcache_write(400, buf);
    bcache_drop();
    fs_ok(pending() == 0 && !on_media(400, 77),
          "bcache_drop discards dirty buffers without writing them (that is its whole job)");
    bcache_sync();
    fs_ok(!on_media(400, 77), "and a later sync does not resurrect them");

    /* --- bounds -------------------------------------------------------------- */
    fs_ok(bcache_read(sim_nblocks, got) != 0, "a read past the device is refused");
    fs_ok(bcache_write(sim_nblocks, buf) != 0, "a write past the device is refused");
    fs_ok(bcache_read(0xFFFFFFFFu, got) != 0, "so is a wildly out-of-range read");

    /* --- shutdown flushes ---------------------------------------------------- */
    pattern(buf, 88);
    bcache_write(410, buf);
    bcache_shutdown();
    blk_flush();
    fs_ok(on_media(410, 88), "shutdown writes back what is still dirty (the CLEAN unmount)");

    sim_close();
    return fs_verdict("fs_cache_test");
}
