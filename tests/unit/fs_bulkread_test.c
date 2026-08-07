/* Reading a file must not cost one device command per block.
 *
 * WHY THIS TEST EXISTS, AND WHAT IT IS THE CONTROL FOR
 * ---------------------------------------------------
 * Measured on the machine before any of this changed (make bench-fs): a
 * virtio-blk read costs ~81 us for 8 sectors and ~120 us for 255 -- a fixed
 * per-command charge of roughly 80 us and a marginal rate under 0.3 us per
 * sector. LogitFS read whole files one 4 KiB block at a time, so opening the
 * 3 MB browser.aex issued 741 commands and spent 51 ms of BKL-held, non-
 * preemptible time on them. Worse, the same read streamed 741 blocks through a
 * 256-buffer cache and evicted everything useful on the way past: a WARM read
 * of that file (54.4 ms) was no faster than a COLD one (51.5 ms).
 *
 * The fix is a run reader (bcache_read_run) and a mapper that walks the
 * indirect chain once (imap_fill), and the claim it makes is not "the code is
 * faster" -- speed on a contended TCG host is weather -- but "the device was
 * asked FEWER TIMES for the same bytes". That is an exact, deterministic,
 * host-checkable number, and it is what this file asserts.
 *
 * THE NEGATIVE CONTROL is the assertion labelled COALESCING below. Against the
 * pre-change filesystem it fails and cannot not fail: reading an N-block file
 * cost N commands, and the bound here is N/64. It compiles and runs against
 * both versions, because it only uses the public read path and the simulated
 * device's own command counter -- nothing about how the coalescing is spelled.
 *
 * Everything else here is the correctness half. A run reader that returns the
 * wrong bytes is not a faster filesystem, and the two ways it can do that are
 * both specific: reading over a block the cache holds DIRTY (the device's copy
 * is stale), and reading over a block the cache holds at all (which is only
 * safe because it is not stale, but must still be served from where the data
 * actually is).
 */

#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"

/* 4096 blocks = 16 MiB: past NDIRECT (12) and past the single-indirect region
 * (12 + 1024 blocks), so the mapper's double-indirect path is exercised by the
 * big file rather than merely present. */
#define NBLOCKS   4096
#define BIGSZ     (900 * LFS_BS)          /* 900 blocks: 3.6 MB, browser-sized */
#define SMALLSZ   (3 * LFS_BS + 100)      /* 3 whole blocks and a short tail */

static uint8_t wbuf[BIGSZ], rbuf[BIGSZ];
static uint8_t blk[LFS_BS], got[LFS_BS];

static void fill(uint8_t *b, int n, int tag)
{
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(tag * 131 + i * 7 + (i >> 8) * 29);
}

static void reset_counters(void) { sim_reads = 0; sim_read_blocks = 0; }

int main(void)
{
    sim_open_n(NBLOCKS);
    fs_ok(logitfs.mount() == 0, "mount");

    /* ---------------------------------------------------------------------
     * The whole-file read path.
     * ------------------------------------------------------------------- */
    fill(wbuf, BIGSZ, 7);
    fs_ok(logitfs.write("/big", wbuf, BIGSZ) == BIGSZ, "write a 900-block file");
    fs_ok(bcache_sync() == 0, "sync it out");

    /* COLD: nothing in the cache, so every block must come from the device.
     * This is the launch case -- an app is read once, right after boot. */
    bcache_drop();
    reset_counters();
    memset(rbuf, 0, BIGSZ);
    int n = logitfs.read("/big", rbuf, BIGSZ);
    unsigned long cold_cmds = sim_reads, cold_blocks = sim_read_blocks;

    fs_ok(n == BIGSZ, "cold read returns the whole file (%d of %d)", n, BIGSZ);
    fs_ok(memcmp(rbuf, wbuf, BIGSZ) == 0, "cold read returns the RIGHT bytes");
    fs_ok(cold_blocks >= 900, "the device really did deliver 900+ blocks (%lu)", cold_blocks);

    /* THE NEGATIVE CONTROL.
     * 900 data blocks at 128 blocks per command is 8 commands, plus a handful
     * for the directory, the indirect blocks and the double-indirect L1. The
     * bound is 900/64 = 14, which is generous against the coalesced version and
     * unreachable for one-command-per-block: that spends 900+.  */
    fs_ok(cold_cmds <= 900 / 64,
          "COALESCING: a 900-block cold read costs %lu device commands, must be <= %d",
          cold_cmds, 900 / 64);

    /* And the ratio itself, stated as the thing it is. */
    fs_ok(cold_blocks / (cold_cmds ? cold_cmds : 1) >= 64,
          "each command carried >= 64 blocks (%lu blocks / %lu commands)",
          cold_blocks, cold_cmds);
    printf("  a 900-block cold read: %lu device commands, %lu blocks (%lu blocks/command)\n",
           cold_cmds, cold_blocks, cold_blocks / (cold_cmds ? cold_cmds : 1));

    /* ---------------------------------------------------------------------
     * A big read must not destroy the cache on its way past.
     *
     * This is the second half of the measured problem and it is separate from
     * the command count: 900 blocks through a 256-buffer pool with an install-
     * everything policy leaves the pool holding 256 blocks of a file nobody will
     * read again, and the metadata that was in there is gone.
     * ------------------------------------------------------------------- */
    bcache_shutdown();
    fs_ok(bcache_init(sim_nblocks, 64) == 0, "re-init the cache with 64 buffers");

    /* Seed the pool with something worth keeping and prove it is resident. */
    for (uint32_t b = 0; b < 8; b++) {
        fill(blk, LFS_BS, 200 + (int)b);
        memcpy(sim_media + (size_t)(NBLOCKS - 16 + b) * LFS_BS, blk, LFS_BS);
        bcache_read(NBLOCKS - 16 + b, got);
    }
    struct bcache_stats st;
    bcache_getstats(&st);
    unsigned long h0 = st.hits;
    bcache_read(NBLOCKS - 16, got);
    bcache_getstats(&st);
    fs_ok(st.hits == h0 + 1, "the seeded block is resident before the stream");

    (void)logitfs.read("/big", rbuf, BIGSZ);      /* the stream goes past */

    bcache_getstats(&st);
    h0 = st.hits;
    bcache_read(NBLOCKS - 16, got);
    bcache_getstats(&st);
    fs_ok(st.hits == h0 + 1,
          "STREAM BYPASS: a 900-block read did not evict the seeded block");
    fs_ok(st.bypassed >= 850, "and it is recorded as bypassed (%lu blocks)", st.bypassed);

    /* ---------------------------------------------------------------------
     * A SMALL read still populates the cache -- the measurement said the cache
     * was worth 14x on a 12 KB app, and bypassing everything would have thrown
     * that away to fix the big case.
     * ------------------------------------------------------------------- */
    fill(wbuf, SMALLSZ, 11);
    fs_ok(logitfs.write("/small", wbuf, SMALLSZ) == SMALLSZ, "write a 3-block file");
    bcache_sync();
    bcache_drop();
    reset_counters();
    fs_ok(logitfs.read("/small", rbuf, SMALLSZ) == SMALLSZ, "cold small read");
    fs_ok(memcmp(rbuf, wbuf, SMALLSZ) == 0, "cold small read is correct");
    unsigned long small_cold = sim_reads;

    reset_counters();
    memset(rbuf, 0, SMALLSZ);
    fs_ok(logitfs.read("/small", rbuf, SMALLSZ) == SMALLSZ, "warm small read");
    fs_ok(memcmp(rbuf, wbuf, SMALLSZ) == 0, "warm small read is correct");
    fs_ok(sim_reads == 0,
          "SMALL FILES STILL CACHE: the warm read touched the device %lu times, must be 0",
          sim_reads);
    fs_ok(small_cold <= 3, "and the cold one cost %lu commands, not one per block", small_cold);

    /* ---------------------------------------------------------------------
     * Correctness of the run reader itself, at the two places it can be wrong.
     * ------------------------------------------------------------------- */
    bcache_shutdown();
    fs_ok(bcache_init(sim_nblocks, 64) == 0, "re-init");

    /* Lay 16 known blocks on media. */
    for (uint32_t b = 0; b < 16; b++) {
        fill(blk, LFS_BS, 400 + (int)b);
        memcpy(sim_media + (size_t)(2000 + b) * LFS_BS, blk, LFS_BS);
    }

    /* (a) a plain run of 16 non-resident blocks: one command, right bytes. */
    reset_counters();
    static uint8_t run16[16 * LFS_BS];
    fs_ok(bcache_read_run(2000, 16, run16) == 0, "run read of 16 blocks");
    fs_ok(sim_reads == 1, "one device command for 16 contiguous blocks (%lu)", sim_reads);
    int allright = 1;
    for (int b = 0; b < 16; b++) {
        fill(blk, LFS_BS, 400 + b);
        if (memcmp(run16 + (size_t)b * LFS_BS, blk, LFS_BS) != 0) allright = 0;
    }
    fs_ok(allright, "and every block landed at the right offset");

    /* (b) a DIRTY block in the middle. The device's copy is stale by
     * construction, so a run reader that read straight over it returns data the
     * filesystem has already overwritten -- silent corruption, and the single
     * most dangerous thing this change could have introduced. */
    bcache_drop();
    fill(blk, LFS_BS, 999);
    fs_ok(bcache_write(2007, blk) == 0, "dirty one block in the middle of the run");
    memset(run16, 0, sizeof run16);
    fs_ok(bcache_read_run(2000, 16, run16) == 0, "run read across a dirty block");
    fs_ok(memcmp(run16 + (size_t)7 * LFS_BS, blk, LFS_BS) == 0,
          "DIRTY MID-RUN: the run reader returned the CACHE's copy, not media's");
    fill(blk, LFS_BS, 406);
    fs_ok(memcmp(run16 + (size_t)6 * LFS_BS, blk, LFS_BS) == 0,
          "and the blocks either side of it are still correct");
    fill(blk, LFS_BS, 408);
    fs_ok(memcmp(run16 + (size_t)8 * LFS_BS, blk, LFS_BS) == 0, "(the one after, too)");

    /* (c) bounds. A run that starts inside the device and ends past it must be
     * refused whole, not truncated: a partial success here is a buffer the
     * caller believes is full. */
    fs_ok(bcache_read_run(sim_nblocks - 4, 8, run16) != 0, "a run running off the end is refused");
    fs_ok(bcache_read_run(sim_nblocks, 1, run16) != 0, "a run starting past the end is refused");
    fs_ok(bcache_read_run(2000, 0, run16) != 0, "a zero-length run is refused");

    /* (d) one block through the run path must equal one block through the
     * single-block path -- the two must not disagree about anything. */
    bcache_drop();
    fs_ok(bcache_read_run(2003, 1, run16) == 0, "one-block run read");
    fs_ok(bcache_read(2003, got) == 0, "one-block plain read");
    fs_ok(memcmp(run16, got, LFS_BS) == 0, "and they agree");

    bcache_sync();
    logitfs_unmount();
    sim_close();
    return fs_verdict("fs_bulkread_test");
}
