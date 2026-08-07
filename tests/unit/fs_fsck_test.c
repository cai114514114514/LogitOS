/* fsck against deliberately damaged images.
 *
 * A journal keeps a crash from leaving a torn transaction behind. It does
 * nothing about the other ways a filesystem goes wrong -- a block the device
 * returned wrong, a bug in the filesystem, an image built by broken tooling,
 * bytes somebody edited. That damage is perfectly transaction-atomic and still
 * nonsense, and only a full scan finds it. So each case below breaks the image
 * in one specific way and demands three things of the checker:
 *
 *   1. it NOTICES, and says which class of problem it is;
 *   2. --repair either fixes it completely (a second pass is clean) or REFUSES,
 *      and refusing is the right answer whenever the damage has more than one
 *      plausible fix;
 *   3. it never makes things worse: everything that was still readable before
 *      the repair is still readable, byte for byte, after it.
 *
 * Point 3 is the one worth being paranoid about. A checker that guesses turns
 * recoverable damage into confident nonsense, which is strictly worse than
 * leaving the filesystem broken and saying so.
 */

#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"
#include "fsck.h"
#include "crc32.h"

#define MAXF 70000
static uint8_t wbuf[MAXF], rbuf[MAXF];
static uint8_t *snap;
static struct lfs_super SB;

static void fill(uint8_t *b, int n, int tag)
{
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(tag * 131 + i * 7 + (i >> 8) * 29);
}

/* --- straight at the media ------------------------------------------------- */
static uint8_t *mblk(uint32_t b) { return sim_media + (size_t)b * LFS_BS; }
static struct lfs_dinode *minode(uint32_t ino)
{
    return (struct lfs_dinode *)(void *)(mblk(SB.inode_start) + (size_t)ino * LFS_INODE_SIZE);
}
static uint8_t *mbitmap(void) { return mblk(SB.bitmap_start); }
static void bit_set_media(uint32_t b)   { mbitmap()[b >> 3] |=  (uint8_t)(1u << (b & 7)); }
static void bit_clr_media(uint32_t b)   { mbitmap()[b >> 3] &= (uint8_t)~(1u << (b & 7)); }
static int  bit_get_media(uint32_t b)   { return (mbitmap()[b >> 3] >> (b & 7)) & 1; }

static int d_read(void *cx, uint32_t blk, void *buf)
{ (void)cx; if (blk >= sim_nblocks) return -1; memcpy(buf, mblk(blk), LFS_BS); return 0; }
static int d_write(void *cx, uint32_t blk, const void *buf)
{ (void)cx; if (blk >= sim_nblocks) return -1; memcpy(mblk(blk), buf, LFS_BS); return 0; }
static int d_sync(void *cx) { (void)cx; return 0; }

static int check(struct fsck_report *r, int repair)
{
    struct fsck_dev d = { d_read, repair ? d_write : NULL, d_sync, NULL, sim_nblocks };
    return fsck_run(&d, repair, r, NULL, NULL);
}

/* --- the healthy image ----------------------------------------------------- */
struct file_spec { const char *path; int n, tag; };
static const struct file_spec FILES[] = {
    { "/one",     4000,  1 },
    { "/two",     60000, 2 },   /* single indirect */
    { "/d/three", 9000,  3 },
    { "/d/four",  100,   4 },
};
#define NFILES ((int)(sizeof FILES / sizeof FILES[0]))

static void build_image(void)
{
    sim_mkfs();
    logitfs.mount();
    logitfs.mkdir("/d");
    for (int i = 0; i < NFILES; i++) {
        fill(wbuf, FILES[i].n, FILES[i].tag);
        logitfs.write(FILES[i].path, wbuf, FILES[i].n);
    }
    logitfs_unmount();
    blk_flush();
    sim_read_super(&SB);
    memcpy(snap, sim_media, (size_t)sim_nblocks * LFS_BS);
}
static void reset(void) { memcpy(sim_media, snap, (size_t)sim_nblocks * LFS_BS); sim_npend = 0; }

/* Every file still readable and correct. `damaged` is a bitmask of the files
 * this particular corruption was aimed at: those may legitimately come back
 * missing or shortened. Every OTHER file must survive byte for byte -- that is
 * the "never make things worse" assertion, and it is the reason this runs after
 * every single repair rather than once at the end. */
static void files_intact(const char *what, unsigned damaged)
{
    fs_ok(logitfs.mount() == 0, "%s: mounts after repair", what);
    for (int i = 0; i < NFILES; i++) {
        int sz = logitfs.size(FILES[i].path);
        if ((damaged >> i) & 1) continue;
        if (sz < 0) {
            fs_ok(0, "%s: %s vanished", what, FILES[i].path);
            continue;
        }
        int ok = 0;
        if (sz == FILES[i].n && logitfs.read(FILES[i].path, rbuf, FILES[i].n) == FILES[i].n) {
            fill(wbuf, FILES[i].n, FILES[i].tag);
            ok = memcmp(rbuf, wbuf, (size_t)FILES[i].n) == 0;
        }
        fs_ok(ok, "%s: %s is still byte-for-byte correct (fsck must not make things worse)",
              what, FILES[i].path);
    }
    logitfs_unmount();
}

/* ino of a name directly under the root, straight off the media. */
static uint32_t ino_of(const char *name)
{
    struct lfs_dinode *root = minode(SB.root_ino);
    uint32_t nblk = (root->size + LFS_BS - 1) / LFS_BS;
    for (uint32_t b = 0; b < nblk && b < LFS_NDIRECT; b++) {
        struct lfs_dirent *de = (struct lfs_dirent *)(void *)mblk(root->direct[b]);
        for (uint32_t j = 0; j < LFS_BS / LFS_DIRENT_SZ; j++)
            if (de[j].name[0] && !strncmp(de[j].name, name, LFS_NAME_MAX - 1)) return de[j].ino;
    }
    return 0;
}

static void expect_clean(const char *what)
{
    struct fsck_report r;
    int rc = check(&r, 0);
    fs_ok(rc == 0 && r.problems == 0, "%s: a second pass finds nothing (%d problem(s) left)",
          what, r.problems);
}

int main(void)
{
    struct fsck_report r;
    sim_open();
    snap = malloc((size_t)sim_nblocks * LFS_BS);
    if (!snap) return 2;
    build_image();

    /* --- 0. a healthy image is clean, and stays clean ---------------------- */
    reset();
    fs_ok(check(&r, 0) == 0 && r.problems == 0,
          "a filesystem the driver just built is clean (%d problem(s))", r.problems);
    fs_ok(check(&r, 1) == 0 && r.fixed == 0, "repairing a clean image changes nothing");
    files_intact("clean", 0);

    /* --- 1. a torn journal -------------------------------------------------- */
    /* A commit record whose block write did not land whole. Every prefix must be
     * rejected, and the target block must be untouched. */
    for (int sectors = 1; sectors < LFS_SPB; sectors++) {
        reset();
        uint32_t tgt = minode(ino_of("one"))->direct[0];
        uint8_t before[LFS_BS];
        memcpy(before, mblk(tgt), LFS_BS);

        uint8_t body[LFS_BS];
        memset(body, 0x5A, LFS_BS);
        memcpy(mblk(SB.log_start + 1), body, LFS_BS);
        uint32_t *h = (uint32_t *)(void *)mblk(SB.log_start);
        memset(h, 0, LFS_BS);
        h[LFS_LOGH_MAGIC] = LFS_LOG_MAGIC; h[LFS_LOGH_GEN] = 3;
        h[LFS_LOGH_COUNT] = 1; h[LFS_LOGH_BCRC] = crc32(body, LFS_BS);
        h[LFS_LOGH_TARGET] = tgt;
        h[LFS_LOGH_HCRC] = lfs_log_hdr_crc(h);
        memset((uint8_t *)h + (size_t)sectors * LFS_SECTOR, 0,
               LFS_BS - (size_t)sectors * LFS_SECTOR);       /* the tail never landed */

        check(&r, 0);
        fs_ok(r.journal_discarded == 1 && r.journal_replayed == 0,
              "torn journal (%d sector(s) landed): reported as discarded, not replayed", sectors);
        check(&r, 1);
        fs_ok(memcmp(mblk(tgt), before, LFS_BS) == 0,
              "torn journal (%d sector(s)): live data untouched", sectors);
        expect_clean("torn journal");
        files_intact("torn journal", 0);
    }

    /* --- 2. a journal record standing over the wrong bodies ------------------ */
    reset();
    {
        uint32_t tgt = minode(ino_of("one"))->direct[0];
        uint8_t before[LFS_BS], bodyA[LFS_BS], bodyB[LFS_BS];
        memcpy(before, mblk(tgt), LFS_BS);
        memset(bodyA, 0x11, LFS_BS);
        memset(bodyB, 0x22, LFS_BS);
        memcpy(mblk(SB.log_start + 1), bodyA, LFS_BS);
        uint32_t *h = (uint32_t *)(void *)mblk(SB.log_start);
        memset(h, 0, LFS_BS);
        h[LFS_LOGH_MAGIC] = LFS_LOG_MAGIC; h[LFS_LOGH_GEN] = 4;
        h[LFS_LOGH_COUNT] = 1; h[LFS_LOGH_BCRC] = crc32(bodyA, LFS_BS);
        h[LFS_LOGH_TARGET] = tgt;
        h[LFS_LOGH_HCRC] = lfs_log_hdr_crc(h);
        memcpy(mblk(SB.log_start + 1), bodyB, LFS_BS);   /* a later transaction's body */

        check(&r, 1);
        fs_ok(r.journal_discarded == 1, "stale journal record: discarded");
        fs_ok(memcmp(mblk(tgt), before, LFS_BS) == 0,
              "stale journal record: live data untouched");
        expect_clean("stale journal");
        files_intact("stale journal", 0);
    }

    /* --- 3. bitmap says FREE, an inode says MINE ---------------------------- */
    /* The dangerous direction: the block gets handed to a second file and the
     * two share it silently. Shown to be real damage first, then repaired. */
    reset();
    {
        uint32_t blk = minode(ino_of("two"))->direct[5];
        bit_clr_media(blk);
        check(&r, 0);
        fs_ok(r.bitmap_missing >= 1, "bitmap missing a referenced block: found (%d)", r.bitmap_missing);

        /* the damage, demonstrated: allocate, and /two is corrupted */
        logitfs.mount();
        fill(wbuf, 20000, 77);
        logitfs.write("/intruder", wbuf, 20000);
        int sz = logitfs.size("/two");
        int corrupted = 0;
        if (sz == 60000 && logitfs.read("/two", rbuf, 60000) == 60000) {
            fill(wbuf, 60000, 2);
            corrupted = memcmp(rbuf, wbuf, 60000) != 0;
        }
        logitfs_unmount();
        fs_ok(corrupted, "a block marked free but referenced really is handed out twice "
                         "(so repairing it is not cosmetic)");

        /* now the repair, on the un-damaged image */
        reset();
        bit_clr_media(blk);
        fs_ok(check(&r, 1) == 0 && r.fixed == r.problems, "bitmap missing: repaired");
        fs_ok(bit_get_media(blk) == 1, "the bit is set again");
        expect_clean("bitmap missing");
        files_intact("bitmap missing", 0);
    }

    /* --- 4. bitmap says USED, nothing references it -------------------------- */
    reset();
    {
        uint32_t leak = SB.total_blocks - 3;
        bit_set_media(leak);
        check(&r, 0);
        fs_ok(r.bitmap_leaked >= 1, "leaked block: found (%d)", r.bitmap_leaked);
        fs_ok(check(&r, 1) == 0, "leaked block: repaired");
        fs_ok(bit_get_media(leak) == 0, "the bit is cleared, the space comes back");
        expect_clean("leaked block");
        files_intact("leaked block", 0);
    }

    /* --- 5. a directory loop -------------------------------------------------- */
    /* /d gains an entry pointing back at itself: the tree is no longer a tree,
     * and a walker that does not detect it never terminates. */
    reset();
    {
        uint32_t dino = ino_of("d");
        struct lfs_dinode *d = minode(dino);
        struct lfs_dirent *de = (struct lfs_dirent *)(void *)mblk(d->direct[0]);
        int slot = (int)(d->size / LFS_DIRENT_SZ);
        de[slot].ino = dino;
        memcpy(de[slot].name, "loop", 5);
        d->size += LFS_DIRENT_SZ;

        check(&r, 0);
        fs_ok(r.dir_loop >= 1, "directory loop: found (%d)", r.dir_loop);
        fs_ok(check(&r, 1) == 0, "directory loop: repaired by cutting the back edge");
        expect_clean("directory loop");
        files_intact("directory loop", 0);
    }

    /* --- 6. a bad link count ------------------------------------------------- */
    /* LogitFS has no hard links, so its link-count invariant is "exactly one
     * dirent names each live inode". Both violations are checked. */
    reset();                                   /* (a) two names, one inode */
    {
        uint32_t one = ino_of("one");
        struct lfs_dinode *root = minode(SB.root_ino);
        struct lfs_dirent *de = (struct lfs_dirent *)(void *)mblk(root->direct[0]);
        int slot = (int)(root->size / LFS_DIRENT_SZ);
        de[slot].ino = one;
        memcpy(de[slot].name, "alias", 6);
        root->size += LFS_DIRENT_SZ;

        check(&r, 0);
        fs_ok(r.multi_linked >= 1, "two dirents naming one inode: found (%d)", r.multi_linked);
        fs_ok(check(&r, 1) == 0, "multi-linked inode: repaired by dropping the extra name");
        expect_clean("multi-linked");
        files_intact("multi-linked", 0);
    }

    reset();                                   /* (b) no name at all: an orphan */
    {
        /* Detach /one by clearing its dirent, leaving the inode allocated. */
        uint32_t one = ino_of("one");
        struct lfs_dinode *root = minode(SB.root_ino);
        struct lfs_dirent *de = (struct lfs_dirent *)(void *)mblk(root->direct[0]);
        for (uint32_t j = 0; j < LFS_BS / LFS_DIRENT_SZ; j++)
            if (de[j].ino == one) { de[j].ino = 0; de[j].name[0] = 0; }

        check(&r, 0);
        fs_ok(r.orphan_inode >= 1, "an inode no dirent names: found (%d)", r.orphan_inode);
        fs_ok(check(&r, 1) == 0, "orphan inode: repaired by freeing it and its blocks");
        fs_ok(minode(one)->type == LFS_T_FREE, "the orphan inode is free again");
        expect_clean("orphan inode");
        files_intact("orphan inode", 1u << 0);          /* /one is legitimately gone */
    }

    /* --- 7. a dirent pointing at a free inode --------------------------------- */
    reset();
    {
        uint32_t one = ino_of("one");
        memset(minode(one), 0, sizeof(struct lfs_dinode));   /* free the inode, keep the name */
        check(&r, 0);
        fs_ok(r.dirent_bad_ino >= 1, "dangling dirent: found (%d)", r.dirent_bad_ino);
        fs_ok(check(&r, 1) == 0, "dangling dirent: repaired by removing the name");
        expect_clean("dangling dirent");
        files_intact("dangling dirent", 1u << 0);
    }

    /* --- 8. one block claimed by two inodes: REFUSE --------------------------- */
    /* There is no correct answer here -- "give it to /one" and "give it to /two"
     * are equally defensible and one of them destroys a file. The only honest
     * behaviour is to report it and decline. */
    reset();
    {
        uint32_t shared = minode(ino_of("two"))->direct[3];
        minode(ino_of("one"))->direct[0] = shared;
        check(&r, 0);
        fs_ok(r.dup_block >= 1, "a block claimed twice: found (%d)", r.dup_block);
        int rc = check(&r, 1);
        fs_ok(rc != 0, "a block claimed twice: --repair REFUSES rather than guessing");
        fs_ok(minode(ino_of("two"))->direct[3] == shared &&
              minode(ino_of("one"))->direct[0] == shared,
              "and it changed neither claimant");
        fs_ok(bit_get_media(shared) == 1,
              "but it did mark the block used, so it is not handed out a third time");
    }

    /* --- 9. an unusable superblock: REFUSE ------------------------------------ */
    reset();
    {
        uint8_t before[LFS_BS];
        memcpy(before, mblk(0), LFS_BS);
        ((uint32_t *)(void *)mblk(0))[0] = 0xDEADBEEF;      /* wrong magic */
        fs_ok(check(&r, 1) != 0 && r.fatal, "a foreign superblock: refused, not 'repaired'");
        fs_ok(memcmp(mblk(0), before + 0, 4) != 0, "(the test's own damage is still there)");

        reset();
        struct lfs_super bad;
        memcpy(&bad, mblk(0), sizeof bad);
        bad.data_start = 2;                                 /* regions overlap */
        memcpy(mblk(0), &bad, sizeof bad);
        fs_ok(check(&r, 1) != 0 && r.fatal, "impossible geometry: refused");

        reset();
        memcpy(&bad, mblk(0), sizeof bad);
        bad.total_blocks = 0x7FFFFFFF;                      /* bigger than the device */
        memcpy(mblk(0), &bad, sizeof bad);
        fs_ok(check(&r, 1) != 0 && r.fatal,
              "a superblock claiming more blocks than the device has: refused");
    }

    /* --- 10. the root is not a directory: REFUSE ------------------------------ */
    reset();
    {
        minode(SB.root_ino)->type = LFS_T_FILE;
        fs_ok(check(&r, 1) != 0 && r.fatal, "root inode not a directory: refused");
    }

    /* --- 11. an inode with a block pointer outside the data area -------------- */
    reset();
    {
        minode(ino_of("one"))->direct[0] = SB.total_blocks + 99;
        check(&r, 0);
        fs_ok(r.bad_blockptr >= 1, "a block pointer past the image: found (%d)", r.bad_blockptr);
        fs_ok(check(&r, 1) == 0, "block pointer past the image: repaired by truncating");
        expect_clean("bad block pointer");
        files_intact("bad block pointer", 1u << 0);
    }

    /* --- 12. a directory size that is not a whole number of dirents ----------- */
    reset();
    {
        minode(ino_of("d"))->size += 17;
        check(&r, 0);
        fs_ok(r.dirent_bad_size >= 1, "ragged directory size: found");
        fs_ok(check(&r, 1) == 0, "ragged directory size: repaired");
        expect_clean("ragged directory size");
        files_intact("ragged directory size", 0);
    }

    /* --- 13. an inode with an unknown type ------------------------------------ */
    reset();
    {
        minode(ino_of("one"))->type = 9;
        check(&r, 0);
        fs_ok(r.bad_inode_type >= 1, "unknown inode type: found");
        fs_ok(check(&r, 1) == 0, "unknown inode type: repaired");
        expect_clean("unknown inode type");
        files_intact("unknown inode type", 1u << 0);
    }

    /* --- 14. timestamps ------------------------------------------------------- */
    reset();
    {
        fs_ok(logitfs.mount() == 0, "timestamps: mount");
        struct lfs_dinode *in = minode(ino_of("two"));
        fs_ok(in->mtime == fsstub_clock,
              "a file written by the driver carries the clock's mtime");
        fs_ok(in->ctime == fsstub_clock && in->atime == fsstub_clock,
              "and ctime/atime too");
        fsstub_clock += 3600;
        fill(wbuf, 5000, 42);
        logitfs.write("/two", wbuf, 5000);
        logitfs_unmount();
        blk_flush();
        in = minode(ino_of("two"));
        fs_ok(in->mtime == fsstub_clock, "rewriting the file moves mtime forward");
        fs_ok(minode(SB.root_ino)->mtime <= fsstub_clock,
              "and the parent directory's mtime is not in the future");
        fsstub_clock -= 3600;
    }

    free(snap);
    sim_close();
    return fs_verdict("fs_fsck_test");
}
