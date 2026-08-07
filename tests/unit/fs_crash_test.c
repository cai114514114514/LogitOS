/* Crash injection for LogitFS, on the host, at EVERY write.
 *
 * The QEMU harnesses (tests/boot/run-fscrash-test.sh) kill the emulator at a
 * randomised moment and get a handful of samples per run. That is worth having
 * -- it is the real kernel on a real virtio device -- but it is not a proof of
 * anything, because the interesting window is a few block writes wide and a
 * random kill almost never lands in it. So this test does the complementary
 * thing: it runs the actual filesystem source against a simulated device and
 * cuts the power at write 1, write 2, write 3, ... every single write of the
 * operation, for several loss patterns each.
 *
 * The device model is the part that makes this mean something (see fs_sim.h): a
 * write is accepted into a volatile cache, only blk_flush() puts it on media,
 * and a power cut lands an ARBITRARY SUBSET of what was pending, with one block
 * possibly torn. A stub that wrote straight through would make every barrier a
 * no-op and this whole file vacuous.
 *
 * What is asserted after every single cut:
 *   - the filesystem mounts;
 *   - fsck finds NOTHING wrong: the bitmap agrees with the inodes, no block is
 *     claimed twice, no dirent dangles, no inode is orphaned;
 *   - every file that was completely written and acknowledged before the cut is
 *     byte-for-byte intact;
 *   - the file the cut interrupted is whole or absent, never partial;
 *   - allocating afterwards does not hand out a block that is already in use --
 *     checked by writing a new file and re-verifying the old ones, because a
 *     doubly-allocated block produces a file of exactly the right LENGTH holding
 *     someone else's bytes, which no length check can see.
 */

#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"
#include "fsck.h"

/* A power cut longjmps out of the middle of the filesystem, so whatever it was
 * holding is leaked by construction. That is what a power cut is; leak checking
 * would only report the simulation working. Memory-error checking stays on. */
const char *__asan_default_options(void) { return "detect_leaks=0"; }

#define MAXF (128 * 1024)

static uint8_t *snap;
static uint8_t  wbuf[MAXF], rbuf[MAXF];

static void snapshot(void) { memcpy(snap, sim_media, (size_t)sim_nblocks * LFS_BS); }
static void restore(void)  { memcpy(sim_media, snap, (size_t)sim_nblocks * LFS_BS); sim_npend = 0; }

/* Content is a function of (tag, offset), so a block delivered to the wrong
 * place shows up as a named first-bad-byte rather than as a shrug. */
static void fill(uint8_t *b, int n, int tag)
{
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(tag * 131 + i * 7 + (i >> 8) * 29);
}

static int mount_fs(void)  { return logitfs.mount(); }
static void clean_umount(void) { logitfs_unmount(); }
static void power_umount(void) { bcache_drop(); logitfs_unmount(); }   /* nothing volatile survives */

static int write_file(const char *path, int n, int tag)
{
    fill(wbuf, n, tag);
    return logitfs.write(path, wbuf, n);
}

/* -1 absent, 0 present and correct, 1 present and WRONG. */
static int check_file(const char *path, int n, int tag)
{
    int sz = logitfs.size(path);
    if (sz < 0) return -1;
    if (sz != n) return 1;
    memset(rbuf, 0, (size_t)n);
    if (logitfs.read(path, rbuf, n) != n) return 1;
    fill(wbuf, n, tag);
    return memcmp(rbuf, wbuf, (size_t)n) ? 1 : 0;
}

/* --- offline fsck straight at the media (the filesystem must be unmounted) -- */
static int m_read(void *cx, uint32_t blk, void *buf)
{
    (void)cx;
    if (blk >= sim_nblocks) return -1;
    memcpy(buf, sim_media + (size_t)blk * LFS_BS, LFS_BS);
    return 0;
}
static int m_write(void *cx, uint32_t blk, const void *buf)
{
    (void)cx;
    if (blk >= sim_nblocks) return -1;
    memcpy(sim_media + (size_t)blk * LFS_BS, buf, LFS_BS);
    return 0;
}
static int m_sync(void *cx) { (void)cx; return 0; }

static int fsck_media(struct fsck_report *r, int repair)
{
    struct fsck_dev d = { m_read, repair ? m_write : NULL, m_sync, NULL, sim_nblocks };
    return fsck_run(&d, repair, r, NULL, NULL);
}

/* Describe the state of the whole filesystem after a cut. */
struct expect { const char *path; int n, tag, may_be_absent; };

static const struct expect BYSTANDERS[] = {
    { "/bys1",   100,   1, 0 },
    { "/bys2",   60000, 2, 0 },
    { "/d/bys3", 9000,  3, 0 },
};
#define NBYS ((int)(sizeof BYSTANDERS / sizeof BYSTANDERS[0]))

static void lay_down_bystanders(void)
{
    fs_ok(mount_fs() == 0, "setup: mount");
    for (int i = 0; i < NBYS; i++) {
        if (BYSTANDERS[i].path[1] == 'd') logitfs.mkdir("/d");
        fs_ok(write_file(BYSTANDERS[i].path, BYSTANDERS[i].n, BYSTANDERS[i].tag)
              == BYSTANDERS[i].n, "setup: write %s", BYSTANDERS[i].path);
    }
    for (int i = 0; i < NBYS; i++)
        fs_ok(check_file(BYSTANDERS[i].path, BYSTANDERS[i].n, BYSTANDERS[i].tag) == 0,
              "setup: %s reads back", BYSTANDERS[i].path);
    clean_umount();
}

/* One operation to interrupt. Returns 0 on success. */
typedef int (*victim_op)(void);

/* 70000 bytes = 18 blocks, past direct[12], so the sweep cuts inside the
 * single-indirect path as well as the direct one. (The double-indirect tree
 * needs >1036 blocks and does not fit this 2 MiB simulated disk; it is covered
 * by tests/boot/run-hugefile-test.sh instead.) */
static int op_write_victim(void)  { return write_file("/victim", 70000, 9) == 70000 ? 0 : -1; }
static int op_mkdir(void)         { return logitfs.mkdir("/vd"); }
static int op_delete_bys1(void)   { return logitfs.del("/bys1"); }
static int op_rename_bys2(void)   { return logitfs.rename("/bys2", "/bys2r"); }
static int op_overwrite_bys2(void){ return write_file("/bys2", 41000, 7) == 41000 ? 0 : -1; }

/* After a cut during `op`, what must be true of each file. */
static void verify_state(const char *what, int k, victim_op op)
{
    struct fsck_report r;
    char msg[160];

    fs_ok(mount_fs() == 0, "%s cut@%d: filesystem must mount", what, k);

    for (int i = 0; i < NBYS; i++) {
        const struct expect *e = &BYSTANDERS[i];
        int st = check_file(e->path, e->n, e->tag);
        int allow_absent = 0, allow_other = 0;
        if (op == op_delete_bys1  && i == 0) allow_absent = 1;
        if (op == op_rename_bys2  && i == 1) allow_absent = 1;   /* it may be /bys2r now */
        if (op == op_overwrite_bys2 && i == 1) allow_other = 1;  /* old or new content */
        if (allow_other) {
            if (st != 0) st = check_file(e->path, 41000, 7);
            snprintf(msg, sizeof msg,
                     "%s cut@%d: %s must be entirely the old content or entirely the new",
                     what, k, e->path);
        } else {
            snprintf(msg, sizeof msg, "%s cut@%d: bystander %s must be intact%s",
                     what, k, e->path, allow_absent ? " or absent" : "");
        }
        fs_ok(st == 0 || (allow_absent && st == -1), "%s", msg);
    }

    if (op == op_write_victim) {
        int st = check_file("/victim", 70000, 9);
        fs_ok(st == 0 || st == -1,
              "%s cut@%d: the interrupted file must be whole or absent, never partial", what, k);
    }
    if (op == op_rename_bys2) {
        int a = check_file("/bys2", 60000, 2), b = check_file("/bys2r", 60000, 2);
        fs_ok((a == 0) != (b == 0),
              "%s cut@%d: a rename must leave the file under exactly one name", what, k);
    }

    /* Allocate again: a block handed out twice shows on the OLD files, not the
     * new one, so the re-verify below is the assertion that matters. */
    write_file("/probe", 5000, 5);
    for (int i = 0; i < NBYS; i++) {
        const struct expect *e = &BYSTANDERS[i];
        if (op == op_delete_bys1 && i == 0) continue;
        if (op == op_rename_bys2 && i == 1) continue;
        if (op == op_overwrite_bys2 && i == 1) continue;
        fs_ok(check_file(e->path, e->n, e->tag) == 0,
              "%s cut@%d: %s still intact after a fresh allocation "
              "(else a block was handed out twice)", what, k, e->path);
    }

    clean_umount();

    if (fsck_media(&r, 0) != 0 || r.problems) {
        char detail[200];
        snprintf(detail, sizeof detail,
                 "%s cut@%d: fsck found %d problem(s) "
                 "(dup-block %d, bitmap missing/leaked %d/%d, dirent %d, loop %d, orphan %d, "
                 "bad-size %d, bad-ptr %d)",
                 what, k, r.problems, r.dup_block, r.bitmap_missing, r.bitmap_leaked,
                 r.dirent_bad_ino, r.dir_loop, r.orphan_inode, r.bad_size, r.bad_blockptr);
        fs_ok(0, "%s", detail);
    } else {
        fs_ok(1, "%s cut@%d: fsck clean", what, k);
    }
}

/* Count the device writes one clean run of `op` costs, so the sweep can cut at
 * every one of them. */
static long measure(victim_op op)
{
    restore();
    sim_budget = -1;
    mount_fs();
    sim_writes = 0;
    op();
    clean_umount();
    long n = sim_writes;
    restore();
    return n;
}

static void sweep(const char *what, victim_op op, int seeds)
{
    long nw = measure(op);
    printf("  %-16s %ld device writes -> %ld cut point(s) x %d loss pattern(s)\n",
           what, nw, nw, seeds);
    for (long k = 1; k <= nw; k++) {
        for (int s = 1; s <= seeds; s++) {
            restore();
            sim_budget = -1;
            if (mount_fs() != 0) { fs_ok(0, "%s cut@%ld: setup mount failed", what, (long)k); continue; }
            sim_budget = k;
            sim_crash_armed = 1;
            if (setjmp(sim_crash_jmp) == 0) {
                op();
                /* Ran to completion without spending the budget: the operation
                 * is shorter than we measured (a delete after the file is gone,
                 * say). Nothing was interrupted, so nothing to check here. */
                sim_budget = -1; sim_crash_armed = 0;
                clean_umount();
                continue;
            }
            /* --- the plug is out --- */
            sim_budget = -1; sim_crash_armed = 0;
            sim_power_cut((uint32_t)(k * 7919 + s), s % 2);   /* half the patterns tear a block */
            power_umount();
            verify_state(what, (int)k, op);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The CLAUDE.md regression: "corrupts after repeated non-snapshot boots".
 *
 * Every boot harness in this tree passes -snapshot, which throws the disk away
 * on exit, so the failure was only ever seen by hand. This reproduces the shape
 * of it deterministically and cheaply: many clean mount/churn/unmount cycles
 * against ONE image, verifying bystanders byte-for-byte and running a full fsck
 * after every cycle. The churn (allocate and free repeatedly) is what shakes a
 * free-block bitmap out of step with the inode table, and the damage never
 * shows on the churned files -- it shows on a bystander written before them.
 * ------------------------------------------------------------------------- */
static void repeated_boots(int boots)
{
    restore();
    for (int b = 1; b <= boots; b++) {
        struct fsck_report r;
        fs_ok(mount_fs() == 0, "boot %d: mount", b);
        for (int i = 0; i < NBYS; i++)
            fs_ok(check_file(BYSTANDERS[i].path, BYSTANDERS[i].n, BYSTANDERS[i].tag) == 0,
                  "boot %d: bystander %s byte-for-byte", b, BYSTANDERS[i].path);
        /* churn: allocate and free, in a different order each boot */
        for (int i = 0; i < 8; i++) {
            char p[32];
            snprintf(p, sizeof p, "/churn%d", (i * 5 + b) % 8);
            write_file(p, 3000 + i * 1700, 40 + i);
            if ((i + b) % 3) logitfs.del(p);
        }
        logitfs.mkdir("/cd");
        logitfs.del("/cd");
        clean_umount();
        if (fsck_media(&r, 0) != 0 || r.problems)
            fs_ok(0, "boot %d: fsck found %d problem(s) (dup %d, bitmap %d/%d, orphan %d)",
                  b, r.problems, r.dup_block, r.bitmap_missing, r.bitmap_leaked, r.orphan_inode);
        else
            fs_ok(1, "boot %d: fsck clean", b);
    }
}

/* ---------------------------------------------------------------------------
 * fsync must mean something.
 *
 * LogitFS commits a whole transaction, barriers and all, inside every mutating
 * VFS call -- so "written and acknowledged" already implies "on media", and
 * that is the strong half. The half worth proving is that it is TRUE: cut the
 * power the instant the write returns, with nothing flushed afterwards, and the
 * data must still be there. The weak half is the unacknowledged write: it may
 * or may not survive, and either way the filesystem must be consistent.
 * ------------------------------------------------------------------------- */
static void fsync_semantics(void)
{
    struct fsck_report r;

    /* acknowledged: survives a cut with no further flush of any kind */
    restore();
    fs_ok(mount_fs() == 0, "fsync: mount");
    fs_ok(write_file("/acked", 22000, 11) == 22000, "fsync: the write returns success");
    fs_ok(logitfs_sync() == 0, "fsync: sync() succeeds");
    sim_power_cut(1234, 0);          /* anything still volatile is lost */
    power_umount();
    fs_ok(mount_fs() == 0, "fsync: mounts after the cut");
    fs_ok(check_file("/acked", 22000, 11) == 0,
          "fsync: an acknowledged, synced write MUST survive the power cut");
    clean_umount();
    fs_ok(fsck_media(&r, 0) == 0 && r.problems == 0, "fsync: consistent afterwards");

    /* Same thing WITHOUT the explicit sync: the commit inside vfs_write already
     * barriered, so this must also survive. If it ever stops surviving, the
     * transaction stopped being synchronous and fsync became load-bearing --
     * which is a design change, not a test failure to paper over. */
    restore();
    mount_fs();
    fs_ok(write_file("/acked2", 17000, 12) == 17000, "fsync: second write returns success");
    sim_power_cut(4321, 0);
    power_umount();
    fs_ok(mount_fs() == 0, "fsync: mounts after the second cut");
    fs_ok(check_file("/acked2", 17000, 12) == 0,
          "fsync: a returned vfs_write is durable without an explicit sync "
          "(the transaction commits before it returns)");
    clean_umount();
    fs_ok(fsck_media(&r, 0) == 0 && r.problems == 0, "fsync: consistent afterwards (2)");

    /* Unacknowledged: interrupted mid-write. May be there or not; must never be
     * partial, and must never leave damage. Covered exhaustively by sweep(),
     * asserted here once more in its own right. */
    restore();
    mount_fs();
    sim_writes = 0;
    sim_budget = 6;
    sim_crash_armed = 1;
    if (setjmp(sim_crash_jmp) == 0) { write_file("/unacked", 70000, 13); }
    sim_budget = -1; sim_crash_armed = 0;
    sim_power_cut(999, 0);
    power_umount();
    fs_ok(mount_fs() == 0, "fsync: mounts after an interrupted write");
    {
        int st = check_file("/unacked", 70000, 13);
        fs_ok(st == 0 || st == -1,
              "fsync: an unacknowledged write is whole or absent, never partial");
    }
    clean_umount();
    fs_ok(fsck_media(&r, 0) == 0 && r.problems == 0,
          "fsync: an unacknowledged write leaves a consistent filesystem either way");
}

int main(int argc, char **argv)
{
    int seeds = 3, boots = 25;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q') { seeds = 1; boots = 8; }

    sim_open();
    snap = malloc((size_t)sim_nblocks * LFS_BS);
    if (!snap) return 2;

    logitfs_set_clock(NULL);            /* the stub RTC; fixed instant */

    printf("laying down bystanders\n");
    lay_down_bystanders();
    snapshot();

    printf("repeated clean boots (the CLAUDE.md corruption regression)\n");
    repeated_boots(boots);

    printf("fsync semantics\n");
    fsync_semantics();

    printf("exhaustive crash injection\n");
    sweep("write",      op_write_victim,   seeds);
    sweep("mkdir",      op_mkdir,          seeds);
    sweep("delete",     op_delete_bys1,    seeds);
    sweep("rename",     op_rename_bys2,    seeds);
    sweep("overwrite",  op_overwrite_bys2, seeds);

    free(snap);
    sim_close();
    return fs_verdict("fs_crash_test");
}
