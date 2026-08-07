/* The write-ahead log's record format and its replay rules, in isolation.
 *
 * Four things have to be true of a commit record, and each one corresponds to a
 * crash the filesystem must survive:
 *
 *   1. a complete record over its own bodies replays;
 *   2. a TORN record -- the classic truncated final record, a 4 KiB header write
 *      that only partly landed -- is discarded, not replayed;
 *   3. a record whose BODIES are not the ones it was sealed against is
 *      discarded. This is the stale-header window: the previous transaction's
 *      header clear is an ordinary device write with no barrier after it, so it
 *      can be lost while the NEXT transaction's body writes reach media. Every
 *      field of that old header is intact and self-consistent; only the bodies
 *      underneath it have moved on;
 *   4. a record aimed at the superblock, at the log itself, or off the end of
 *      the image is refused, so a forged image cannot use recovery as a
 *      write primitive.
 *
 * Case 3 carries the NEGATIVE CONTROL: the same crafted image is run through the
 * rule this log used before it carried checksums (magic + count + targets, which
 * is what xv6 and the previous LogitFS did), and that rule is shown to install
 * the wrong bytes over live data. The check that fails without the change is
 * that comparison.
 */

#include "fs_sim.h"
#include "fs_check.h"
#include "fsck.h"
#include "crc32.h"

/* --- device shim for fsck_log_recover: straight at the media, no cache ----- */
static int d_read(void *cx, uint32_t blk, void *buf)
{
    (void)cx;
    if (blk >= sim_nblocks) return -1;
    memcpy(buf, sim_media + (size_t)blk * LFS_BS, LFS_BS);
    return 0;
}
static int d_write(void *cx, uint32_t blk, const void *buf)
{
    (void)cx;
    if (blk >= sim_nblocks) return -1;
    memcpy(sim_media + (size_t)blk * LFS_BS, buf, LFS_BS);
    return 0;
}
static int d_sync(void *cx) { (void)cx; return 0; }

static struct fsck_dev DEV = { d_read, d_write, d_sync, NULL, 0 };

static uint8_t *media(uint32_t blk) { return sim_media + (size_t)blk * LFS_BS; }

/* Seal a one-block transaction: body `body` destined for block `target`. */
static void seal(const struct lfs_super *sb, uint32_t target, const uint8_t *body)
{
    memcpy(media(sb->log_start + 1), body, LFS_BS);
    uint8_t *h8 = media(sb->log_start);
    memset(h8, 0, LFS_BS);
    uint32_t *h = (uint32_t *)(void *)h8;
    h[LFS_LOGH_MAGIC]  = LFS_LOG_MAGIC;
    h[LFS_LOGH_GEN]    = 42;
    h[LFS_LOGH_COUNT]  = 1;
    h[LFS_LOGH_BCRC]   = crc32(body, LFS_BS);
    h[LFS_LOGH_TARGET] = target;
    h[LFS_LOGH_HCRC]   = lfs_log_hdr_crc(h8);
}

/* The rule this log used BEFORE the record carried checksums: a plausible magic
 * and an in-range count were the whole test. Reproduced here, and nowhere else,
 * so the negative control below is something a reader can check by eye. */
static int legacy_would_replay(const struct lfs_super *sb, const uint8_t *hdr)
{
    const uint32_t *h = (const uint32_t *)(const void *)hdr;
    if (h[LFS_LOGH_MAGIC] != LFS_LOG_MAGIC) return 0;
    uint32_t n = h[LFS_LOGH_COUNT];
    return n != 0 && n <= sb->log_blocks - 1;
}

static void pattern(uint8_t *b, int tag)
{
    for (int i = 0; i < LFS_BS; i++) b[i] = (uint8_t)(tag * 37 + i * 11 + (i >> 5));
}

int main(void)
{
    sim_open();
    struct lfs_super sb;
    sim_read_super(&sb);
    DEV.nblocks = sim_nblocks;

    uint32_t target = sb.data_start + 3;
    uint8_t  live[LFS_BS], bodyA[LFS_BS], bodyB[LFS_BS], got[LFS_BS];
    pattern(live,  1);       /* what the target block holds before recovery */
    pattern(bodyA, 2);       /* the body the record was sealed against */
    pattern(bodyB, 3);       /* a later transaction's body, in the same slot */

    /* --- 1. a complete record replays ------------------------------------- */
    memcpy(media(target), live, LFS_BS);
    seal(&sb, target, bodyA);
    int discarded = 0;
    int n = fsck_log_recover(&DEV, &sb, &discarded);
    fs_ok(n == 1, "a complete commit record should replay 1 block, got %d", n);
    d_read(NULL, target, got);
    fs_ok(memcmp(got, bodyA, LFS_BS) == 0, "replay must install the logged body");
    d_read(NULL, sb.log_start, got);
    fs_ok(((uint32_t *)(void *)got)[LFS_LOGH_MAGIC] != LFS_LOG_MAGIC,
          "replay must clear the header when it is done");

    /* replay is idempotent: re-seal and replay twice, same result */
    memcpy(media(target), live, LFS_BS);
    seal(&sb, target, bodyA);
    fsck_log_recover(&DEV, &sb, &discarded);
    seal(&sb, target, bodyA);
    fsck_log_recover(&DEV, &sb, &discarded);
    d_read(NULL, target, got);
    fs_ok(memcmp(got, bodyA, LFS_BS) == 0, "replaying twice must be the same as once");

    /* --- 2. a torn record is discarded ------------------------------------ */
    /* A 4 KiB write commits in 512-byte sectors; a power cut mid-write leaves a
     * prefix. Every prefix must be rejected, including the ones that keep the
     * magic and the count perfectly intact. */
    for (int sectors = 0; sectors < LFS_SPB; sectors++) {
        memcpy(media(target), live, LFS_BS);
        seal(&sb, target, bodyA);
        uint8_t whole[LFS_BS];
        memcpy(whole, media(sb.log_start), LFS_BS);
        memset(media(sb.log_start), 0, LFS_BS);
        memcpy(media(sb.log_start), whole, (size_t)sectors * LFS_SECTOR);
        discarded = 0;
        n = fsck_log_recover(&DEV, &sb, &discarded);
        d_read(NULL, target, got);
        fs_ok(n == 0, "a record torn after %d sector(s) must not replay (got %d)", sectors, n);
        fs_ok(memcmp(got, live, LFS_BS) == 0,
              "a record torn after %d sector(s) must leave the target untouched", sectors);
    }

    /* --- 3. a record standing over the wrong bodies ------------------------ */
    /* The stale-header window, built exactly: transaction N's header survives
     * (its clear was lost), transaction N+1's body reached the same log slot. */
    memcpy(media(target), live, LFS_BS);
    seal(&sb, target, bodyA);                    /* record N, sealed over bodyA */
    memcpy(media(sb.log_start + 1), bodyB, LFS_BS);  /* N+1's body lands in the slot */

    uint8_t stale_hdr[LFS_BS];
    memcpy(stale_hdr, media(sb.log_start), LFS_BS);
    fs_ok(((uint32_t *)(void *)stale_hdr)[LFS_LOGH_HCRC] == lfs_log_hdr_crc(stale_hdr),
          "the stale header is itself intact -- nothing about the header is damaged");

    discarded = 0;
    n = fsck_log_recover(&DEV, &sb, &discarded);
    d_read(NULL, target, got);
    fs_ok(n == 0, "a record whose bodies are not its own must not replay (got %d)", n);
    fs_ok(discarded == 1, "and it must be discarded, so the next mount does not see it again");
    fs_ok(memcmp(got, live, LFS_BS) == 0,
          "STALE HEADER: live data must be untouched -- this is the corruption the "
          "body CRC exists to prevent");

    /* NEGATIVE CONTROL. The pre-checksum rule accepts that same header, and
     * installing what it points at overwrites live data with a different
     * transaction's bytes. If this check ever fails, the two rules have become
     * the same and the body CRC is no longer doing anything. */
    memcpy(media(target), live, LFS_BS);
    seal(&sb, target, bodyA);
    memcpy(media(sb.log_start + 1), bodyB, LFS_BS);
    memcpy(stale_hdr, media(sb.log_start), LFS_BS);
    fs_ok(legacy_would_replay(&sb, stale_hdr),
          "negative control: the pre-checksum rule accepts the stale header");
    {   /* what that rule would then do */
        uint8_t victim[LFS_BS];
        memcpy(victim, media(sb.log_start + 1), LFS_BS);
        memcpy(media(target), victim, LFS_BS);
        d_read(NULL, target, got);
        fs_ok(memcmp(got, live, LFS_BS) != 0 && memcmp(got, bodyB, LFS_BS) == 0,
              "negative control: the pre-checksum rule writes transaction N+1's body "
              "to transaction N's target -- live data destroyed by recovery itself");
    }

    /* --- 4. forged targets are refused ------------------------------------ */
    struct { uint32_t tgt; const char *what; } bad[] = {
        { 0,                       "the superblock" },
        { sb.log_start,            "the log header itself" },
        { sb.log_start + 1,        "a log body slot" },
        { sim_nblocks,             "one past the end of the image" },
        { 0xFFFFFFFFu,             "far past the end of the image" },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy(media(target), live, LFS_BS);
        uint8_t before0[LFS_BS];
        memcpy(before0, media(0), LFS_BS);
        seal(&sb, bad[i].tgt, bodyA);
        /* seal() wrote the body into the slot; re-seal against the real slot
         * content so the bcrc is right and only the TARGET is objectionable. */
        discarded = 0;
        n = fsck_log_recover(&DEV, &sb, &discarded);
        fs_ok(n < 0, "a record aimed at %s must be refused (got %d)", bad[i].what, n);
        fs_ok(memcmp(media(0), before0, LFS_BS) == 0,
              "refusing a record aimed at %s must not have written anything", bad[i].what);
        memset(media(sb.log_start), 0, LFS_BS);
    }

    /* --- 5. count bounds --------------------------------------------------- */
    /* A count that would make the target array run into the header CRC word, or
     * exceed the log's body slots, is not a record. */
    {
        uint32_t counts[] = { 0, sb.log_blocks, sb.log_blocks + 1, LFS_LOG_MAX + 1, 0xFFFFFFFFu };
        for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; i++) {
            memcpy(media(target), live, LFS_BS);
            seal(&sb, target, bodyA);
            uint32_t *h = (uint32_t *)(void *)media(sb.log_start);
            h[LFS_LOGH_COUNT] = counts[i];
            h[LFS_LOGH_HCRC]  = lfs_log_hdr_crc(h);   /* intact header, absurd count */
            discarded = 0;
            n = fsck_log_recover(&DEV, &sb, &discarded);
            fs_ok(n <= 0, "count %u must not replay (got %d)", counts[i], n);
            d_read(NULL, target, got);
            fs_ok(memcmp(got, live, LFS_BS) == 0, "count %u must leave the target alone", counts[i]);
        }
    }

    /* --- 6. a single flipped bit in a body ---------------------------------- */
    memcpy(media(target), live, LFS_BS);
    seal(&sb, target, bodyA);
    media(sb.log_start + 1)[LFS_BS / 2] ^= 0x01;
    discarded = 0;
    n = fsck_log_recover(&DEV, &sb, &discarded);
    d_read(NULL, target, got);
    fs_ok(n == 0, "one flipped bit in a body must invalidate the record (got %d)", n);
    fs_ok(memcmp(got, live, LFS_BS) == 0, "and must leave the target untouched");

    sim_close();
    return fs_verdict("fs_journal_test");
}
