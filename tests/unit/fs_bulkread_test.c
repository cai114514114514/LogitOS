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

    /* ---------------------------------------------------------------------
     * PARTIAL READS -- read(2)'s shape, which this filesystem did not have.
     *
     * ->read is ALL OR NOTHING: `if (size > (uint32_t)max) return -1`. Asking
     * for the first 12 KiB of a 36 KiB file returned -1, not 12 KiB. That is
     * measured below rather than described, because it is the invariant the new
     * op had to leave alone: twenty callers size a buffer from vfs_size and
     * read a short return as failure.
     *
     * ->pread is the addition, and it walks the SAME block-mapping loop --
     * inode_read is now a bounds check in front of inode_pread. Two walkers
     * that can disagree is the failure this filesystem is least able to see,
     * because both produce a file of exactly the right LENGTH.
     *
     * These checks are skipped, loudly, on the b9b33ef negative-control build,
     * whose logitfs.c predates ->pread and leaves the member NULL.
     * ------------------------------------------------------------------- */
    bcache_shutdown();
    fs_ok(bcache_init(sim_nblocks, 64) == 0, "re-init the cache for the partial-read section");

    if (!logitfs.pread) {
        printf("  [pread] SKIPPED: this build's logitfs has no ->pread member "
               "(the b9b33ef negative-control source). 0 partial-read checks ran.\n");
    } else {
        static uint8_t pbuf[4 * LFS_BS];

        /* THE CASE THAT NAMED THE BUG: the first 12 KiB of a 36 KiB file. */
        #define P36 (36 * 1024)
        fill(wbuf, P36, 36);
        fs_ok(logitfs.write("/p36", wbuf, P36) == P36, "write a 36 KiB file");
        bcache_sync();

        fs_ok(logitfs.read("/p36", pbuf, 12 * 1024) == -1,
              "ALL-OR-NOTHING PRESERVED: ->read of 12 KiB from a 36 KiB file is still refused");
        memset(pbuf, 0, sizeof pbuf);
        fs_ok(logitfs.pread("/p36", pbuf, 12 * 1024, 0) == 12 * 1024,
              "->pread of the first 12 KiB returns 12288");
        fs_ok(memcmp(pbuf, wbuf, 12 * 1024) == 0, "and they are the right 12288 bytes");
        memset(pbuf, 0, sizeof pbuf);
        fs_ok(logitfs.pread("/p36", pbuf, 12 * 1024, 12 * 1024) == 12 * 1024,
              "->pread of the SECOND 12 KiB returns 12288");
        fs_ok(memcmp(pbuf, wbuf + 12 * 1024, 12 * 1024) == 0, "and those are right too");

        /* The whole file through the new op must equal the whole file through
         * the old one. If these ever differ, one of the two walkers is wrong. */
        fill(wbuf, BIGSZ, 7);                    /* /big was written with tag 7 */
        memset(rbuf, 0, BIGSZ);
        fs_ok(logitfs.pread("/big", rbuf, BIGSZ, 0) == BIGSZ,
              "->pread at offset 0 for the whole 900-block file");
        fs_ok(memcmp(rbuf, wbuf, BIGSZ) == 0, "and it agrees with ->read byte for byte");

        /* UNALIGNED HEAD: an offset inside a block. The head is staged through
         * blk_buf precisely so the bytes before `off` never reach the caller. */
        memset(pbuf, 0xAA, sizeof pbuf);
        fs_ok(logitfs.pread("/big", pbuf, 100, LFS_BS + 1) == 100, "100 bytes at offset 4097");
        fs_ok(memcmp(pbuf, wbuf + LFS_BS + 1, 100) == 0, "the unaligned head lands correctly");
        fs_ok(pbuf[100] == 0xAA, "and nothing was written past what was asked for");

        /* HEAD AND TAIL INSIDE ONE BLOCK -- no whole block at all. */
        memset(pbuf, 0, sizeof pbuf);
        fs_ok(logitfs.pread("/big", pbuf, 7, 5000) == 7, "7 bytes wholly inside one block");
        fs_ok(memcmp(pbuf, wbuf + 5000, 7) == 0, "and they are the right 7");

        /* HEAD + WHOLE BLOCKS + TAIL, the three-region case. */
        memset(pbuf, 0, sizeof pbuf);
        fs_ok(logitfs.pread("/big", pbuf, 3 * LFS_BS + 5, LFS_BS + 1) == 3 * LFS_BS + 5,
              "head + 3 whole blocks + tail");
        fs_ok(memcmp(pbuf, wbuf + LFS_BS + 1, 3 * LFS_BS + 5) == 0, "all three regions agree");

        /* THE NDIRECT SEAM. Block 12 is the first one behind the indirect
         * block, so a read that straddles it exercises both mapper paths in one
         * call -- and an off-by-one there returns the right LENGTH of the wrong
         * bytes, which is the failure mode a length check cannot see. */
        memset(pbuf, 0, sizeof pbuf);
        long seam = (long)12 * LFS_BS - 10;
        fs_ok(logitfs.pread("/big", pbuf, 20, seam) == 20, "20 bytes across the NDIRECT seam");
        fs_ok(memcmp(pbuf, wbuf + seam, 20) == 0, "and they are the right 20");

        /* PAST THE READ_RUN CHUNK. inode_pread's MIDDLE loop asks the mapper
         * for at most read_run (128) blocks at a time, so a request longer than
         * that takes the loop round more than once -- the one place a stale
         * base index would show up. 129 blocks, aligned. */
        static uint8_t bigslice[129 * LFS_BS];
        long boff = (long)200 * LFS_BS;
        memset(bigslice, 0, sizeof bigslice);
        fs_ok(logitfs.pread("/big", bigslice, sizeof bigslice, boff) == (int)sizeof bigslice,
              "129 blocks (past one READ_RUN chunk) in a single ->pread");
        fs_ok(memcmp(bigslice, wbuf + boff, sizeof bigslice) == 0,
              "and the second chunk is not a repeat of the first");

        /* CLAMPING AT END OF FILE, and what lies past it. */
        memset(pbuf, 0, sizeof pbuf);
        fs_ok(logitfs.pread("/big", pbuf, 4096, BIGSZ - 10) == 10,
              "a request straddling EOF returns only what exists (10)");
        fs_ok(memcmp(pbuf, wbuf + BIGSZ - 10, 10) == 0, "and those 10 are right");
        fs_ok(logitfs.pread("/big", pbuf, 4096, BIGSZ) == 0, "at EOF: 0, not an error");
        fs_ok(logitfs.pread("/big", pbuf, 4096, BIGSZ + 1000000) == 0, "past EOF: 0, not an error");
        fs_ok(logitfs.pread("/big", pbuf, 0, 0) == 0, "a zero-length request is 0");

        /* REFUSALS. A negative offset or length is a caller bug, not a clamp. */
        fs_ok(logitfs.pread("/big", pbuf, 100, -1) == -1, "a negative offset is refused");
        fs_ok(logitfs.pread("/big", pbuf, -1, 0) == -1, "a negative length is refused");
        fs_ok(logitfs.pread("/nosuchfile", pbuf, 100, 0) == -1, "a missing file is refused");

        /* THE COMMAND COUNT, which is what the 64 KiB descriptor window rests
         * on: an aligned 64 KiB slice is sixteen file-consecutive blocks and
         * must cost ONE device command, not sixteen.
         *
         * The metadata is warmed on purpose first, and the number is worse
         * without saying so. A bare bcache_drop() charges this read for the
         * directory block AND the file's indirect block as well -- MEASURED:
         * 3 commands and 18 blocks for a 16-block slice, which is a true number
         * about a cold path and the wrong answer to "what does one window
         * refill cost". A descriptor refills its window many times and resolves
         * its path once. So: drop everything, read ONE block at 299 (in the
         * single-indirect region, so it pulls in the directory and the indirect
         * block and nothing this read will want), then measure. Blocks 300..315
         * are still untouched, so the run itself is genuinely cold. */
        bcache_drop();
        static uint8_t win[16 * LFS_BS];
        long woff = (long)300 * LFS_BS;
        (void)logitfs.pread("/big", win, LFS_BS, woff - LFS_BS);   /* warm: dir + indirect */
        reset_counters();
        fs_ok(logitfs.pread("/big", win, sizeof win, woff) == (int)sizeof win,
              "a cold aligned 64 KiB ->pread");
        fs_ok(memcmp(win, wbuf + woff, sizeof win) == 0, "with the right bytes");
        fs_ok(sim_reads == 1,
              "WINDOW COST: 64 KiB aligned costs %lu device command(s), must be 1", sim_reads);
        printf("  a cold aligned 64 KiB ->pread: %lu device command(s), %lu blocks\n",
               sim_reads, sim_read_blocks);
    }

    bcache_sync();
    logitfs_unmount();
    sim_close();
    return fs_verdict("fs_bulkread_test");
}
