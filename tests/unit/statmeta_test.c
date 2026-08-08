/* Does a mode SURVIVE, or does it just look right?
 *
 * c/fs/vfs_meta.h has said since it was written that its records live in RAM and
 * do not survive a reboot, and that logitfs could take ownership by filling in
 * getattr/setattr. It now has. The only claim worth testing about that is the
 * one a working system cannot demonstrate: a mode that is read back correctly
 * within one boot proves nothing at all, because the RAM store did that too.
 *
 * So every assertion here that matters happens ACROSS A REMOUNT. The simulated
 * device (tests/unit/fsstub/fs_sim.h) keeps its media across logitfs_unmount()
 * plus logitfs.mount(), and drops the buffer cache and the in-RAM bitmap and
 * inode table -- which is exactly the part of a reboot that can lose something.
 *
 * THE NEGATIVE CONTROL, and it is the whole reason this file is shaped this way.
 * Built with -DLOGITFS_NO_ATTR the filesystem registers neither hook, and this
 * test falls back to what a plausible stat looks like: mode 0644 for files and
 * 0755 for directories, the size correct, the times set to a boot constant.
 * Everything still WORKS -- size, type, existence, ls -l would look right -- and
 * only the cross-remount assertions fail. If they did not fail, they were never
 * testing durability. `make test-statmeta-negctl` succeeds when this build
 * fails, which is the only way to know the suite can see the difference.
 */

#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"

/* What a defaulting stat would invent for a timestamp. DELIBERATELY NOT
 * fsstub_clock (1750000000), which is the instant the simulated RTC stamps real
 * inodes with: if the two matched, "mtime is the file's own and not a boot
 * constant" would be true in both builds and the assertion would be measuring
 * nothing. Two distinct constants are what make the negative control visible. */
#define BOOT_TIME 1700000000LL

static uint8_t buf[64 * 1024];

static void fill(uint8_t *b, int n, int tag)
{ for (int i = 0; i < n; i++) b[i] = (uint8_t)(tag * 131 + i * 7); }

/* ------------------------------------------------------------------------
 * The one accessor. Under the real build it is the filesystem's getattr; under
 * the negative control it is a plausible default that reads nothing durable.
 * Returns 0 on success. `have_hooks` is what the two builds differ in.
 * --------------------------------------------------------------------- */
static int have_hooks(void) { return logitfs.getattr && logitfs.setattr; }

static int meta_get(const char *path, struct vattr *a)
{
    if (have_hooks()) return logitfs.getattr(path, a);

    /* NEGATIVE CONTROL: everything works, nothing durable is read. */
    memset(a, 0, sizeof *a);
    int sz = logitfs.size(path);
    int isdir = (sz < 0 && logitfs.count(path) >= 0);
    if (sz < 0 && !isdir) return -1;
    a->type    = isdir ? VT_DIR : VT_REG;
    a->mode    = isdir ? 0755 : 0644;      /* plausible, and nobody chose it */
    a->nlink   = 1;
    a->blksize = LFS_BS;
    a->size    = sz > 0 ? (uint64_t)sz : 0;
    a->blocks  = (a->size + 511) / 512;
    a->atime = a->mtime = a->ctime = BOOT_TIME;   /* "times set to boot time" */
    a->flags   = 0;
    return 0;
}

static int meta_set(const char *path, const struct vattr *a)
{
    if (have_hooks()) return logitfs.setattr(path, a);
    return 0;                    /* accepted and discarded -- the old chmod */
}

static int set_mode(const char *path, unsigned mode, unsigned uid, unsigned gid)
{
    struct vattr a;
    if (meta_get(path, &a) != 0) return -1;
    a.mode = mode; a.uid = uid; a.gid = gid;
    return meta_set(path, &a);
}

/* A remount: everything volatile goes, the media stays. This is the reboot. */
static void remount(void)
{
    logitfs_unmount();
    bcache_drop();
    fs_ok(logitfs.mount() == 0, "the filesystem mounts again after an unmount");
}

/* A POWER CUT, not a clean unmount: the buffer cache is DROPPED before the
 * filesystem writes anything back, so only what a barrier already put on media
 * survives. A chmod that only reached the cache dies here. */
static void power_cycle(void)
{
    bcache_drop();
    logitfs_unmount();
    fs_ok(logitfs.mount() == 0, "the filesystem mounts after a power cut");
}

int main(void)
{
    sim_open();
    logitfs_set_clock(NULL);
    fs_ok(logitfs.mount() == 0, "mount");

    printf("%s\n", have_hooks()
        ? "logitfs owns its metadata (getattr/setattr registered)"
        : "NEGATIVE CONTROL: no hooks -- stat returns a plausible default");

    /* --- what a fresh file reports ------------------------------------- */
    fill(buf, 3000, 1);
    fs_ok(logitfs.write("/a.bin", buf, 3000) == 3000, "write /a.bin");

    struct vattr a;
    fs_ok(meta_get("/a.bin", &a) == 0, "getattr on a file that exists");
    fs_ok(a.type == VT_REG, "it is a regular file");
    fs_ok(a.size == 3000, "size is the real size (%llu)", (unsigned long long)a.size);
    fs_ok(a.mode == 0644, "a file nobody chmod'd reports the 0644 default");

    /* THE DISTINCTION THE WHOLE LINE IS ABOUT. The mode above is 0644 in both
     * builds and in both cases it is correct -- what differs is whether the
     * caller can tell that nobody chose it. */
    fs_ok(!(a.flags & VA_STORED),
          "...and says so: VA_STORED is CLEAR, so 0644 is a default not a choice");
    fs_ok((a.flags & VA_INO) != 0, "the inode number is real (VA_INO)");
    fs_ok((a.flags & VA_TIMES) != 0, "the timestamps came off the medium (VA_TIMES)");
    fs_ok(a.mtime != BOOT_TIME,
          "mtime is the file's own, not a boot constant (%lld)", (long long)a.mtime);

    /* --- set one, and read it back in the SAME boot -------------------- */
    fs_ok(set_mode("/a.bin", 0741, 7, 9) == 0, "chmod 0741 / chown 7:9");
    fs_ok(meta_get("/a.bin", &a) == 0, "getattr after the chmod");
    fs_ok(a.mode == 0741, "same boot: mode reads back 0741 (got 0%o)", a.mode);
    fs_ok(a.uid == 7 && a.gid == 9, "same boot: owner reads back 7:9");
    fs_ok((a.flags & VA_STORED) != 0, "same boot: VA_STORED is now SET");

    /* Note what has been proved so far: NOTHING. The RAM store passed every
     * line above too, for as long as it existed. The next block is the test. */

    /* --- ACROSS A REMOUNT ---------------------------------------------- */
    remount();
    fs_ok(meta_get("/a.bin", &a) == 0, "the file is still there after a remount");
    fs_ok(a.size == 3000, "remount: the CONTENTS survived (size 3000)");
    fs_ok(a.mode == 0741,
          "REMOUNT: the mode survived (0741, got 0%o) -- this is the claim", a.mode);
    fs_ok(a.uid == 7 && a.gid == 9,
          "REMOUNT: the owner survived (7:9, got %u:%u)", a.uid, a.gid);
    fs_ok((a.flags & VA_STORED) != 0, "REMOUNT: still marked as a stored record");
    fs_ok((a.flags & VA_DURABLE) != 0,
          "REMOUNT: VA_DURABLE -- the record is claimed to be on the medium");

    /* --- and across a POWER CUT, which is the stronger claim ------------ */
    fs_ok(set_mode("/a.bin", 0600, 3, 4) == 0, "chmod 0600 / chown 3:4");
    power_cycle();
    fs_ok(meta_get("/a.bin", &a) == 0, "the file survived the power cut");
    fs_ok(a.mode == 0600,
          "POWER CUT: the mode reached MEDIA, not just the cache (0600, got 0%o)", a.mode);
    fs_ok(a.uid == 3 && a.gid == 4, "POWER CUT: the owner reached media (3:4)");

    /* --- a mode of 0000 is a real mode, not "unset" --------------------- */
    /* This is why the on-disk field carries a presence bit. If "unset" were
     * encoded as zero, chmod 000 would be indistinguishable from never having
     * been set, and a file the owner deliberately locked would come back 0644
     * after a reboot -- silently readable by everyone. */
    fs_ok(logitfs.write("/locked.bin", buf, 16) == 16, "write /locked.bin");
    fs_ok(set_mode("/locked.bin", 0, 0, 0) == 0, "chmod 000");
    remount();
    fs_ok(meta_get("/locked.bin", &a) == 0, "/locked.bin survived");
    fs_ok(a.mode == 0,
          "REMOUNT: chmod 000 stayed 000 (got 0%o) -- 0 is a mode, not 'unset'", a.mode);
    fs_ok((a.flags & VA_STORED) != 0,
          "REMOUNT: ...and is still marked stored, which is what tells them apart");

    /* --- a bystander nobody touched still defaults ---------------------- */
    fs_ok(logitfs.write("/plain.bin", buf, 100) == 100, "write /plain.bin");
    remount();
    fs_ok(meta_get("/plain.bin", &a) == 0, "/plain.bin survived");
    fs_ok(a.mode == 0644, "REMOUNT: an untouched file still reports 0644");
    fs_ok(!(a.flags & VA_STORED),
          "REMOUNT: ...with VA_STORED clear, so a reader knows it is the default");

    /* --- directories carry a mode too ----------------------------------- */
    fs_ok(logitfs.mkdir("/d") == 0, "mkdir /d");
    fs_ok(meta_get("/d", &a) == 0 && a.type == VT_DIR, "/d is a directory");
    fs_ok(a.mode == 0755, "a fresh directory reports the 0755 default");
    fs_ok(set_mode("/d", 0700, 5, 5) == 0, "chmod 0700 on a directory");
    remount();
    fs_ok(meta_get("/d", &a) == 0, "/d survived");
    fs_ok(a.mode == 0700, "REMOUNT: a directory's mode survived (got 0%o)", a.mode);
    fs_ok(a.type == VT_DIR, "REMOUNT: ...and it is still a directory");

    /* --- inode numbers actually identify a file ------------------------- */
    struct vattr b;
    fs_ok(meta_get("/a.bin", &a) == 0 && meta_get("/plain.bin", &b) == 0, "two files");
    fs_ok(a.ino != b.ino, "two files have different inode numbers");
    struct vattr a2;
    remount();
    fs_ok(meta_get("/a.bin", &a2) == 0, "/a.bin after a remount");
    fs_ok(a2.ino == a.ino, "REMOUNT: the inode number is stable across a mount");

    /* --- the contents are still byte-for-byte, after all of that -------- */
    /* A metadata change that quietly ate a data block would produce a file of
     * exactly the right LENGTH holding somebody else's bytes, which the size
     * assertions above cannot see. */
    memset(buf, 0, 3000);
    fs_ok(logitfs.read("/a.bin", buf, 3000) == 3000, "read /a.bin back");
    {
        uint8_t want[3000];
        fill(want, 3000, 1);
        fs_ok(memcmp(buf, want, 3000) == 0,
              "byte-for-byte: five chmods and five remounts did not touch the data");
    }

    logitfs_unmount();
    sim_close();
    return fs_verdict("statmeta_test");
}
