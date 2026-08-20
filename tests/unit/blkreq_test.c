/* The block layer's request engine, on the host.
 *
 * WHY THIS EXISTS AT ALL, AND WHY THE STORAGE GATES DO NOT COVER IT.
 *
 * `make test-fs-host` -- 1,744 crash-injection checks and all the rest -- does
 * not compile c/drivers/block/blkdev.c and never has. It builds against
 * tests/unit/fsstub/blkdev.h, a five-function stub over a simulated disk, which
 * is exactly right for testing a filesystem and means the storage suite is
 * BLIND to everything below blk_read(). So when `struct blk_ops` grew
 * submit/poll on 2026-08-20, the engine those gates rely on had no host gate of
 * its own, and the only place the new state machine ran was inside QEMU.
 *
 * This compiles the REAL blkdev.c and drives it with fake drivers, because the
 * properties worth checking are properties of the ENGINE and not of any disk:
 *
 *   - a driver with only read/write/flush is complete when submit returns
 *   - a driver with submit/poll is NOT, and a request that needs several device
 *     commands must not report done after the first
 *   - a partition's request reaches the driver with the parent's absolute LBA
 *   - a second submitter on a busy medium drives the first request to
 *     completion rather than issuing on top of it  <-- the one with a control
 *   - an async request that would need the single shared bounce buffer is
 *     refused as BLK_E_NODMA, and a synchronous one bounces as it always did
 *
 * THE FAKE DRIVERS ARE THE ORACLE. Each one records what it was actually asked
 * for -- LBA, count, direction, and whether a second command arrived while one
 * was outstanding -- so an assertion can be about what reached the device, not
 * about what the caller believes. A test that only checked return codes would
 * pass against an engine that quietly issued two commands at once, which is
 * precisely the failure asynchrony introduces.
 *
 *   make test-blk-async          (tests/mem.mk)
 *   make test-blk-async-negctl   -DBLK_NO_INTERLOCK, must FAIL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "blkdev.h"

/* ---------------------------------------------------------------- harness -- */
static int checks, failures;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

static void ckeq(long long got, long long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL: %s -- got %lld, want %lld\n", what, got, want);
    }
}

/* ------------------------------------------------- the symbols blkdev needs --
 * blkdev.c links against four drivers and a printf. None of them is under test
 * here, and blk_init() -- the only thing that calls them -- is never invoked:
 * every device in this file is registered by hand, which is what lets a test
 * describe a two-partition disk that no QEMU flag can produce.
 * ------------------------------------------------------------------------- */
volatile int g_ata_busy = 0;
int  ata_busy(void) { return g_ata_busy; }
int  ata_read(uint32_t lba, uint8_t n, void *b)        { (void)lba;(void)n;(void)b; return -1; }
int  ata_write(uint32_t lba, uint8_t n, const void *b) { (void)lba;(void)n;(void)b; return -1; }
int  ata_flush(void) { return -1; }
int  ata_identify(uint64_t *n, char *m) { (void)n;(void)m; return -1; }
int  ahci_init(void) { return 0; }
int  ahci_disk_count(void) { return 0; }
int  nvme_present(void) { return 0; }
int  nvme_init(void) { return -1; }
int  nvme_busy(void) { return 0; }
uint64_t nvme_capacity(void) { return 0; }
int  nvme_blk_submit(struct blk_req *r) { (void)r; return -1; }
int  nvme_blk_poll(struct blk_req *r)   { (void)r; return 1; }
int  virtio_blk_present(void) { return 0; }
int  virtio_blk_init(void) { return -1; }
uint64_t virtio_blk_capacity(void) { return 0; }
int  virtio_blk_read(uint64_t lba, uint32_t n, void *b)        { (void)lba;(void)n;(void)b; return -1; }
int  virtio_blk_write(uint64_t lba, uint32_t n, const void *b) { (void)lba;(void)n;(void)b; return -1; }
int  virtio_blk_flush(void) { return -1; }
void kprintf(const char *fmt, ...) { (void)fmt; }

/* part.c is real, portable code and could have been linked in -- it is stubbed
 * instead because part_scan() is reached only from blk_init(), which this file
 * never calls, and linking it would put a second subject in a gate whose whole
 * claim is about the request engine. `PART_NONE` is what a disk with no table
 * reports, so if blk_init() ever DID run here it would take the honest path. */
struct part_table;
int part_scan(int (*rd)(void *, uint64_t, uint32_t, void *), void *ctx,
              uint64_t dev_sectors, struct part_table *t)
{ (void)rd; (void)ctx; (void)dev_sectors; (void)t; return -1; }
const char *part_scheme_name(int scheme) { (void)scheme; return "none"; }
void part_guid_str(const uint8_t guid[16], char out[37]) { (void)guid; out[0] = 0; }

extern uint64_t blk_hosttest_dma_limit;

/* --------------------------------------------------------- the fake medium --
 * 4096 sectors of RAM with a distinct byte pattern per sector, so a read that
 * lands on the WRONG sector is caught by content and not only by a count. A
 * partition offset that is silently dropped reads the right number of bytes
 * from the wrong place, which is the whole reason the pattern is per-sector.
 * ------------------------------------------------------------------------- */
#define DISK_SECTORS 4096u
static uint8_t g_disk[DISK_SECTORS * BLK_SECTOR];

static void disk_fill(void)
{
    for (unsigned s = 0; s < DISK_SECTORS; s++)
        memset(g_disk + (size_t)s * BLK_SECTOR, (int)(s & 0xFF), BLK_SECTOR);
}
static int sector_is(const uint8_t *p, unsigned s)
{
    for (int i = 0; i < BLK_SECTOR; i++)
        if (p[i] != (uint8_t)(s & 0xFF)) return 0;
    return 1;
}

/* ------------------------------------------------------- driver A: SYNCHRONOUS
 * read/write/flush only -- the ATA PIO / virtio-blk shape. The engine has to
 * synthesise submit/poll for it, and the request must be complete the moment
 * submit returns. */
struct sync_stats { int reads, writes, flushes; uint64_t last_lba; uint32_t last_count; };
static struct sync_stats S;

static int sync_read(void *ctx, uint64_t lba, uint32_t n, void *buf)
{
    (void)ctx;
    if (lba + n > DISK_SECTORS) return -1;
    S.reads++; S.last_lba = lba; S.last_count = n;
    memcpy(buf, g_disk + (size_t)lba * BLK_SECTOR, (size_t)n * BLK_SECTOR);
    return 0;
}
static int sync_write(void *ctx, uint64_t lba, uint32_t n, const void *buf)
{
    (void)ctx;
    if (lba + n > DISK_SECTORS) return -1;
    S.writes++; S.last_lba = lba; S.last_count = n;
    memcpy(g_disk + (size_t)lba * BLK_SECTOR, buf, (size_t)n * BLK_SECTOR);
    return 0;
}
static int sync_flush(void *ctx) { (void)ctx; S.flushes++; return 0; }
static const struct blk_ops sync_ops = {
    .read = sync_read, .write = sync_write, .flush = sync_flush
};

/* -------------------------------------------------------- driver B: ASYNC ----
 * The AHCI/NVMe shape: submit issues ONE chunk and returns, poll finishes it
 * after `latency` looks, and a request larger than `chunk_max` takes several
 * commands. `outstanding` is the property this whole file exists to protect --
 * a second submit while one is in flight sets `overlap`, and no return code
 * anywhere would otherwise reveal it.
 */
struct async_stats {
    int  submits, polls, commands, flushes;
    int  outstanding, overlap;
    int  latency;                 /* polls before a command completes */
    uint32_t chunk_max;           /* sectors per device command */
    uint64_t cmd_lba[64];
    uint32_t cmd_count[64];
    int  ncmd;
    int  fail_at_cmd;             /* -1 = never; else the command that reports an error */
};
static struct async_stats A;

static int async_issue(struct blk_req *r)
{
    if (A.outstanding) A.overlap++;
    A.outstanding = 1;
    A.commands++;
    if (r->op == BLK_OP_FLUSH) { A.flushes++; r->chunk = 0; }
    else {
        uint32_t n = r->count - r->done;
        if (n > A.chunk_max) n = A.chunk_max;
        r->chunk = n;
        if (A.ncmd < 64) { A.cmd_lba[A.ncmd] = r->dev_lba + r->done; A.cmd_count[A.ncmd] = n; }
        A.ncmd++;
    }
    r->tag = 0;                                  /* polls remaining on this command */
    return 0;
}

static int async_submit(void *ctx, struct blk_req *r)
{
    (void)ctx;
    A.submits++;
    return async_issue(r);
}

static int async_poll(void *ctx, struct blk_req *r)
{
    (void)ctx;
    A.polls++;
    if ((int)r->tag < A.latency) { r->tag++; return 0; }     /* still running */
    A.outstanding = 0;

    if (A.fail_at_cmd >= 0 && A.commands - 1 == A.fail_at_cmd) { r->status = -1; return 1; }

    /* The command really moves the bytes, so a wrong cursor shows up as wrong
     * CONTENT and not only as a wrong count. */
    if (r->op == BLK_OP_READ)
        memcpy((uint8_t *)r->buf + (size_t)r->done * BLK_SECTOR,
               g_disk + (size_t)(r->dev_lba + r->done) * BLK_SECTOR,
               (size_t)r->chunk * BLK_SECTOR);
    else if (r->op == BLK_OP_WRITE)
        memcpy(g_disk + (size_t)(r->dev_lba + r->done) * BLK_SECTOR,
               (const uint8_t *)r->buf + (size_t)r->done * BLK_SECTOR,
               (size_t)r->chunk * BLK_SECTOR);

    r->done += r->chunk;
    if (r->op != BLK_OP_FLUSH && r->done < r->count) { async_issue(r); return 0; }
    r->status = 0;
    return 1;
}
static const struct blk_ops async_ops = { .submit = async_submit, .poll = async_poll };

static void async_reset(int latency, uint32_t chunk_max)
{
    memset(&A, 0, sizeof A);
    A.latency = latency;
    A.chunk_max = chunk_max;
    A.fail_at_cmd = -1;
}

/* ============================================================================ */

int main(void)
{
    /* 70, not 64, and sized EXACTLY to the largest transfer any case below
     * makes. Case 9 reads 70 sectors on purpose -- one bounce-buffer-full plus
     * a remainder -- so a 64-sector buffer here overflowed by exactly 6
     * sectors, which ASan caught as a 3072-byte global-buffer-overflow inside
     * blk_rw's read-back memcpy. Nothing was wrong with blkdev.c; the harness
     * was the bug, which is the failure mode this tree's own notes put first.
     *
     * Exact rather than generous on purpose: slack would let the next case
     * that grows past it write into the padding and pass, and the whole reason
     * this file is built with ASan is that it is the only witness to a wrong
     * length in a path whose every other symptom is silence. */
    static uint8_t buf[70 * BLK_SECTOR];
    struct blkdev *sd, *ad, *p1;

    disk_fill();
    blk_hosttest_dma_limit = ~0ull;          /* everything reachable, for now */

    sd = blk_register("sync0",  &sync_ops,  NULL, DISK_SECTORS);
    ad = blk_register("async0", &async_ops, NULL, DISK_SECTORS);
    ck(sd && ad, "both fake devices registered");

    /* A partition of the async disk, exactly as scan_partitions builds one:
     * same ops, same ctx, an absolute start LBA and a shorter length. */
    p1 = blk_register("async0p1", ad->ops, ad->ctx, 100);
    p1->parent = ad;
    p1->start  = 1000;
    p1->part_index = 1;
    ck(p1 != NULL, "partition registered");

    /* -- 1. the synchronous fallback: done when submit returns ------------- */
    memset(&S, 0, sizeof S);
    {
        struct blk_req r;
        blk_req_init(&r, sd, BLK_OP_READ, 7, 2, buf);
        ckeq(blk_submit(&r), 0, "sync submit accepted");
        ckeq(r.state, BLK_REQ_DONE, "sync request is DONE the moment submit returns");
        ckeq(blk_poll(&r), 1, "poll on a finished request says 1");
        ckeq(r.status, 0, "sync request succeeded");
        ckeq(S.reads, 1, "the sync driver saw exactly one read");
        ck(sector_is(buf, 7) && sector_is(buf + BLK_SECTOR, 8), "sync read landed on lba 7,8");
        ck(sd->inflight == NULL, "the medium was released");
    }

    /* -- 2. the async driver: NOT done until poll says so ------------------ */
    async_reset(/*latency*/3, /*chunk_max*/DISK_SECTORS);
    {
        struct blk_req r;
        memset(buf, 0, sizeof buf);
        blk_req_init(&r, ad, BLK_OP_READ, 11, 2, buf);
        ckeq(blk_submit(&r), 0, "async submit accepted");
        ckeq(r.state, BLK_REQ_INFLIGHT, "async request is IN FLIGHT after submit");
        ck(ad->inflight == &r, "the medium records the request");
        ckeq(blk_poll(&r), 0, "poll 1: still running");
        ckeq(blk_poll(&r), 0, "poll 2: still running");
        ckeq(blk_poll(&r), 0, "poll 3: still running");
        ckeq(blk_poll(&r), 1, "poll 4: complete");
        ckeq(r.status, 0, "async request succeeded");
        ck(ad->inflight == NULL, "the medium was released on completion");
        ck(sector_is(buf, 11), "async read landed on lba 11");
        ckeq(A.overlap, 0, "no overlapping commands");
    }

    /* -- 3. chunking: one request, several device commands ----------------- */
    async_reset(1, 8);                       /* 8 sectors per command */
    {
        memset(buf, 0, sizeof buf);
        ckeq(blk_dev_read(ad, 100, 20, buf), 0, "chunked read succeeded");
        ckeq(A.commands, 3, "20 sectors at 8/command is 3 commands");
        ckeq(A.submits, 1, "and ONE submit -- the rest were issued from poll");
        ckeq((long long)A.cmd_lba[0], 100, "command 1 starts at 100");
        ckeq(A.cmd_count[0], 8, "command 1 is 8 sectors");
        ckeq((long long)A.cmd_lba[1], 108, "command 2 starts at 108");
        ckeq((long long)A.cmd_lba[2], 116, "command 3 starts at 116");
        ckeq(A.cmd_count[2], 4, "command 3 carries the remaining 4");
        ck(sector_is(buf, 100) && sector_is(buf + 19 * BLK_SECTOR, 119),
           "first and last sector of the chunked read are right");
        ckeq(A.overlap, 0, "chunking never overlaps commands");
    }

    /* -- 4. a chunk that FAILS fails the whole request --------------------- */
    async_reset(0, 8);
    A.fail_at_cmd = 1;                       /* the second command errors */
    {
        ckeq(blk_dev_read(ad, 200, 20, buf), -1, "a failed chunk fails the request");
        ckeq(A.commands, 2, "and stops there -- no third command was issued");
        ck(ad->inflight == NULL, "the medium is released after a failure");
    }

    /* -- 5. the partition offset reaches the driver ------------------------ */
    async_reset(0, DISK_SECTORS);
    {
        memset(buf, 0, sizeof buf);
        ckeq(blk_dev_read(p1, 5, 1, buf), 0, "partition read succeeded");
        ckeq((long long)A.cmd_lba[0], 1005, "the driver saw the ABSOLUTE lba 1000+5");
        ck(sector_is(buf, 1005 & 0xFF), "and returned that sector's content");
    }

    /* -- 6. bounds: the partition is a fence, not a hint ------------------- */
    async_reset(0, DISK_SECTORS);
    {
        ckeq(blk_dev_read(p1, 99, 2, buf), -1, "a read crossing the partition end is refused");
        ckeq(blk_dev_read(p1, 100, 1, buf), -1, "a read starting past the end is refused");
        ckeq(blk_dev_read(p1, 0, 0, buf), -1, "a zero-length read is refused");
        ckeq(A.commands, 0, "and NOTHING reached the device");

        struct blk_req r;
        blk_req_init(&r, p1, BLK_OP_READ, 99, 2, buf);
        ckeq(blk_submit(&r), BLK_E_ARG, "blk_submit reports the refusal as BLK_E_ARG");
        ckeq(r.state, BLK_REQ_DONE, "a refused request is DONE, not left in flight");
        ckeq(blk_poll(&r), 1, "so a caller polling it does not wait forever");
    }

    /* -- 7. THE INTERLOCK. A second submitter drives the first request. ----
     * This is the case the whole design turns on, and the one -DBLK_NO_INTERLOCK
     * removes. `r1` is an async request left in flight; the synchronous read
     * that follows must finish r1 first and must not put a second command on
     * the medium. */
    async_reset(2, DISK_SECTORS);
    {
        static uint8_t b1[BLK_SECTOR], b2[BLK_SECTOR];
        struct blk_req r1;
        blk_req_init(&r1, ad, BLK_OP_READ, 300, 1, b1);
        r1.async = 1;
        ckeq(blk_submit(&r1), 0, "the async request is in flight");
        ck(ad->inflight == &r1, "and the medium says so");

        /* Someone else's read, on a PARTITION of the same disk -- the queue
         * belongs to the medium, so this must still serialise. */
        ckeq(blk_dev_read(p1, 5, 1, b2), 0, "the second reader succeeded");
        ckeq(A.overlap, 0, "no second command was issued on top of the first");
        ckeq(blk_poll(&r1), 1, "the async request was completed by the other thread");
        ckeq(r1.status, 0, "and completed successfully");
        ck(sector_is(b1, 300 & 0xFF), "its data is correct");
        ck(sector_is(b2, 1005 & 0xFF), "and so is the second reader's");
    }

    /* -- 8. BLK_E_NODMA: an async request may not use the shared bounce ---- */
    async_reset(0, DISK_SECTORS);
    blk_hosttest_dma_limit = 0;              /* nothing is reachable now */
    {
        unsigned long before = blk_async_refusals();
        struct blk_req r;
        blk_req_init(&r, ad, BLK_OP_READ, 10, 1, buf);
        r.async = 1;
        ckeq(blk_submit(&r), BLK_E_NODMA, "an async request that would bounce is refused");
        ckeq(r.status, BLK_E_NODMA, "and says so in its status");
        ckeq(blk_poll(&r), 1, "a refused async request does not wait forever");
        ckeq(A.commands, 0, "nothing reached the device");
        ck(ad->inflight == NULL, "and the medium was never claimed");
        ckeq((long long)(blk_async_refusals() - before), 1, "the refusal was counted");
    }

    /* -- 9. ... but a SYNCHRONOUS one bounces, in 64-sector pieces --------- */
    async_reset(0, DISK_SECTORS);
    {
        memset(buf, 0, sizeof buf);
        ckeq(blk_dev_read(ad, 400, 70, buf), 0, "the bounced read succeeded");
        ckeq(A.commands, 2, "70 sectors through a 64-sector bounce is 2 commands");
        ckeq(A.cmd_count[0], 64, "the first piece fills the bounce buffer");
        ckeq(A.cmd_count[1], 6, "the second carries the remainder");
        ck(sector_is(buf, 400) && sector_is(buf + 69 * BLK_SECTOR, 469),
           "and every byte arrived in the caller's buffer, in order");

        memset(buf, 0xA5, 3 * BLK_SECTOR);
        ckeq(blk_dev_write(ad, 500, 3, buf), 0, "the bounced write succeeded");
        for (int i = 0; i < 3 * BLK_SECTOR; i++)
            if (g_disk[500 * BLK_SECTOR + i] != 0xA5) { ck(0, "bounced write reached the disk"); break; }
        ck(g_disk[503 * BLK_SECTOR] == (uint8_t)(503 & 0xFF), "and did not run past its end");
    }
    blk_hosttest_dma_limit = ~0ull;

    /* -- 10. flush is an op, not a side door ------------------------------- */
    async_reset(1, DISK_SECTORS);
    {
        ckeq(blk_dev_flush(ad), 0, "flush through the async driver succeeded");
        ckeq(A.flushes, 1, "the driver saw exactly one flush");
        ckeq(A.submits, 1, "issued through submit like everything else");
    }
    memset(&S, 0, sizeof S);
    {
        ckeq(blk_dev_flush(sd), 0, "flush through the sync driver succeeded");
        ckeq(S.flushes, 1, "and reached its flush op");
    }

    /* -- 11. the no-preempt flag is balanced ------------------------------- */
    ckeq(g_ata_busy, 0, "every synchronous wait gave the no-preempt flag back");

    /* -- 12. A REQUEST THAT FITS THE DRIVER'S BOUND IS ONE COMMAND ---------
     *
     * This is the property c/drivers/block/ahci.c's NCQ note turns on, pinned
     * here rather than left to a boot observation. NCQ has exactly two possible
     * sources of a second outstanding tag: two requests on one medium (case 7
     * shows the engine forbids that, and the -DBLK_NO_INTERLOCK control shows
     * the three checks that notice), or one request split into several commands
     * -- which is this. If the engine never splits a request that fits, the
     * queue depth is 1 and a 32-tag controller has nothing to hold.
     *
     * The bound used is AHCI's real one: AHCI_PRDT(8) * AHCI_PRD_MAX(4 MiB) is
     * 65,536 sectors and the LBA48 sector-count field is the tighter of the two
     * at 65,535. The largest transfer anything in this tree asks for is a
     * coalesced whole-file read at the 12 MiB load ceiling = 24,576 sectors,
     * comfortably under it -- so on the real driver this is one command, and the
     * assertion below is that the ENGINE is what makes that true rather than an
     * accident of size. The fake disk is 4,096 sectors, so the request here is
     * the whole of it; what is being pinned is "fits => 1", not the constant. */
    async_reset(0, DISK_SECTORS);
    A.chunk_max = 65535u;                      /* the AHCI per-command bound */
    {
        static uint8_t big[DISK_SECTORS * BLK_SECTOR];
        ckeq(blk_dev_read(ad, 0, DISK_SECTORS, big), 0, "a whole-disk read succeeded");
        ckeq(A.commands, 1, "a request inside the driver's bound is ONE command");
        ckeq(A.cmd_count[0], (int)DISK_SECTORS, "and it carried every sector");
        ckeq(A.overlap, 0, "with nothing ever overlapping it");
        ck(sector_is(big, 0) && sector_is(big + (DISK_SECTORS - 1) * BLK_SECTOR,
                                          DISK_SECTORS - 1),
           "first and last sector both correct");
    }

    printf("%s: %d checks, %d failures\n",
           failures ? "BLK-ASYNC FAILED" : "BLK-ASYNC OK", checks, failures);
    return failures ? 1 : 0;
}
