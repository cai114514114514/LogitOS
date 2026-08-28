/* A read-only LogitFS reader, one instance per block device. See lfsro.h for
 * why this is a second reader of a format that already has one. */

#include <stdint.h>
#include <stddef.h>
#include "lfsro.h"
#include "logitfs_fmt.h"
#include "blkdev.h"
#include "vfs_path.h"
#include "kprintf.h"
#include "spinlock.h"

void *memcpy(void *, const void *, size_t);

#define LFSRO_MAXFS 2

struct lro {
    int   used;
    char  name[24];
    struct blkdev *dev;
    struct lfs_super sb;
    struct filesystem fs;
    /* THE LOCK IS PER INSTANCE, and that is the whole point of the instance.
     * `blk`/`dblk`/`ind`/`namebuf` used to be justified as "every read is
     * synchronous and under the BKL, so one buffer is enough" -- true, and true
     * because of a global lock three files away. They are one buffer per MOUNT
     * now, serialised by this lock across a whole operation, for the same
     * reason logitfs.c's are: bmap() leaves a block in `ind` that the caller
     * then indexes, and dir_lookup() re-reads `dblk` per block while the walk
     * above it still holds a dirent from the previous one. Two threads on one
     * mount interleave those; two mounts never share them. */
    spinlock_t lock;
    uint8_t  blk[LFS_BS];        /* one block of staging, per mount */
    uint8_t  dblk[LFS_BS];       /* a second, for directory scans that also need
                                  * to fetch an inode mid-walk */
    uint32_t ind[LFS_PPB];
    char  namebuf[LFS_NAME_MAX + 2];
};

/* The pool itself is shared: two concurrent lfsro_create() calls scan for a free
 * slot and would claim the same one -- the same "the hazard is the slot claim"
 * shape the WM's window table has. The critical section is a scan of one int
 * per instance and the `used = 1` that ends it, and it must cover both. */
static struct lro pool[LFSRO_MAXFS];
static spinlock_t pool_lock = SPINLOCK_INIT;

static struct lro *self(struct filesystem *f) { return (struct lro *)f->priv; }

static int rd(struct lro *L, uint32_t blk, void *buf)
{
    if (blk >= L->sb.total_blocks) return -1;      /* block numbers come off the
                                                    * medium and are untrusted */
    return blk_dev_read(L->dev, (uint64_t)blk * LFS_SPB, LFS_SPB, buf);
}

static int read_inode(struct lro *L, uint32_t ino, struct lfs_dinode *out)
{
    if (ino >= L->sb.inode_count) return -1;
    uint32_t b = L->sb.inode_start + ino / LFS_IPB;
    if (rd(L, b, L->blk) < 0) return -1;
    memcpy(out, L->blk + (ino % LFS_IPB) * LFS_INODE_SIZE, sizeof *out);
    return 0;
}

/* The n'th data block of an inode: direct, then single indirect, then double.
 * Returns 0 for a hole or an out-of-range index. */
static uint32_t bmap(struct lro *L, const struct lfs_dinode *ino, uint32_t n)
{
    if (n < LFS_NDIRECT) return ino->direct[n];
    n -= LFS_NDIRECT;
    if (n < LFS_PPB) {
        if (!ino->indirect || rd(L, ino->indirect, L->ind) < 0) return 0;
        return L->ind[n];
    }
    n -= LFS_PPB;
    if (n < (uint32_t)LFS_PPB * LFS_PPB) {
        if (!ino->double_indirect || rd(L, ino->double_indirect, L->ind) < 0) return 0;
        uint32_t l2 = L->ind[n / LFS_PPB];
        if (!l2 || rd(L, l2, L->ind) < 0) return 0;
        return L->ind[n % LFS_PPB];
    }
    return 0;
}

/* Find `name` in the directory inode `dino`. Returns its inode number, or
 * LFS_NOINO. */
#define LFSRO_NOINO 0xFFFFFFFFu

static int name_eq(const char *on_disk, const char *want)
{
    /* Never read past the fixed on-disk field: a forged image may omit the
     * terminator. */
    for (int i = 0; i < LFS_NAME_MAX; i++) {
        if (on_disk[i] != want[i]) return 0;
        if (!on_disk[i]) return 1;
    }
    return want[LFS_NAME_MAX] == 0;
}

static uint32_t dir_lookup(struct lro *L, const struct lfs_dinode *dino, const char *name)
{
    uint32_t nblk = (dino->size + LFS_BS - 1) / LFS_BS;
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t phys = bmap(L, dino, b);
        if (!phys || rd(L, phys, L->dblk) < 0) continue;
        uint32_t nent = LFS_BS / LFS_DIRENT_SZ;
        for (uint32_t i = 0; i < nent; i++) {
            if (b * (LFS_BS / LFS_DIRENT_SZ) + i >= dino->size / LFS_DIRENT_SZ) break;
            struct lfs_dirent *de = (struct lfs_dirent *)(L->dblk + i * LFS_DIRENT_SZ);
            if (!de->name[0]) continue;
            if (name_eq(de->name, name)) return de->ino;
        }
    }
    return LFSRO_NOINO;
}

/* Walk an absolute, canonical, fs-relative path to its inode. */
static uint32_t path_ino(struct lro *L, const char *path, struct lfs_dinode *out)
{
    struct lfs_dinode ino;
    uint32_t cur = L->sb.root_ino;
    if (read_inode(L, cur, &ino) < 0) return LFSRO_NOINO;

    int i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        char comp[LFS_NAME_MAX + 1];
        int n = 0;
        while (path[i] && path[i] != '/') {
            if (n >= LFS_NAME_MAX) return LFSRO_NOINO;
            comp[n++] = path[i++];
        }
        comp[n] = 0;
        if (ino.type != LFS_T_DIR) return LFSRO_NOINO;
        uint32_t next = dir_lookup(L, &ino, comp);
        if (next == LFSRO_NOINO) return LFSRO_NOINO;
        if (read_inode(L, next, &ino) < 0) return LFSRO_NOINO;
        cur = next;
    }
    if (out) *out = ino;
    return cur;
}

/* --- ops ---------------------------------------------------------------- */

static int lr_mount_locked(struct filesystem *f)
{
    struct lro *L = self(f);
    if (!L->dev) return -1;
    uint8_t sec[LFS_BS];
    if (blk_dev_read(L->dev, 0, LFS_SPB, sec) < 0) return -1;
    memcpy(&L->sb, sec, sizeof L->sb);
    if (L->sb.magic != LFS_MAGIC) {
        kprintf("[lfsro] %s: no LogitFS superblock\n", L->name);
        return -1;
    }
    if (L->sb.block_size != LFS_BS || !L->sb.total_blocks || !L->sb.inode_count) {
        kprintf("[lfsro] %s: superblock geometry rejected\n", L->name);
        return -1;
    }
    kprintf("[lfsro] mounted %s read-only: v%d, %d blocks, %d inodes\n",
            L->name, (int)L->sb.version, (int)L->sb.total_blocks, (int)L->sb.inode_count);
    return 0;
}

/* The only op with no wrapper below, and deliberately: it touches nothing.
 * Named here so its absence from that list reads as a decision. */
static void lr_umount(struct filesystem *f) { (void)f; }

static int lr_size_locked(struct filesystem *f, const char *path)
{
    struct lro *L = self(f);
    struct lfs_dinode ino;
    if (path_ino(L, path, &ino) == LFSRO_NOINO) return -1;
    return ino.type == LFS_T_FILE ? (int)ino.size : -1;
}

/* `max` bytes from byte `off`; short at end of file, 0 at or past it.
 *
 * This reader already walked the file a block at a time through a single
 * staging buffer, so the offset form is the same loop with the file position
 * carried rather than assumed to start at zero -- which is why lr_read is now
 * this function at offset 0 instead of a second copy of the walk. */
static int lr_pread_locked(struct filesystem *f, const char *path, void *buf, int max, long long off)
{
    struct lro *L = self(f);
    struct lfs_dinode ino;
    if (max < 0 || off < 0) return -1;
    if (path_ino(L, path, &ino) == LFSRO_NOINO || ino.type != LFS_T_FILE) return -1;
    if ((uint64_t)off >= (uint64_t)ino.size) return 0;
    uint64_t avail = (uint64_t)ino.size - (uint64_t)off;
    int want = avail < (uint64_t)max ? (int)avail : max;
    int done = 0;
    while (done < want) {
        uint64_t pos = (uint64_t)off + (uint64_t)done;
        uint32_t phys = bmap(L, &ino, (uint32_t)(pos / LFS_BS));
        int o = (int)(pos % LFS_BS);
        int n = LFS_BS - o;
        if (n > want - done) n = want - done;
        if (!phys || rd(L, phys, L->blk) < 0) return done ? done : -1;
        memcpy((char *)buf + done, L->blk + o, (size_t)n);
        done += n;
    }
    return done;
}

static int lr_read_locked(struct filesystem *f, const char *path, void *buf, int max)
{
    return lr_pread_locked(f, path, buf, max, 0);
}

static int lr_count_locked(struct filesystem *f, const char *dir)
{
    struct lro *L = self(f);
    struct lfs_dinode ino;
    if (path_ino(L, dir, &ino) == LFSRO_NOINO || ino.type != LFS_T_DIR) return -1;
    int n = 0;
    uint32_t nent = ino.size / LFS_DIRENT_SZ;
    for (uint32_t i = 0; i < nent; i++) {
        uint32_t phys = bmap(L, &ino, i / (LFS_BS / LFS_DIRENT_SZ));
        if (!phys || rd(L, phys, L->dblk) < 0) continue;
        struct lfs_dirent *de =
            (struct lfs_dirent *)(L->dblk + (i % (LFS_BS / LFS_DIRENT_SZ)) * LFS_DIRENT_SZ);
        if (de->name[0]) n++;
    }
    return n;
}

/* Fetch entry `idx` of `dir` into `de`. Shared by ent_name/size/is_dir so the
 * three cannot disagree about what the n'th entry is. */
static int nth_ent(struct lro *L, const char *dir, int idx, struct lfs_dirent *de)
{
    struct lfs_dinode ino;
    if (path_ino(L, dir, &ino) == LFSRO_NOINO || ino.type != LFS_T_DIR) return -1;
    int n = 0;
    uint32_t nent = ino.size / LFS_DIRENT_SZ;
    for (uint32_t i = 0; i < nent; i++) {
        uint32_t phys = bmap(L, &ino, i / (LFS_BS / LFS_DIRENT_SZ));
        if (!phys || rd(L, phys, L->dblk) < 0) continue;
        struct lfs_dirent *d =
            (struct lfs_dirent *)(L->dblk + (i % (LFS_BS / LFS_DIRENT_SZ)) * LFS_DIRENT_SZ);
        if (!d->name[0]) continue;
        if (n++ == idx) { *de = *d; return 0; }
    }
    return -1;
}

static const char *lr_ent_name_locked(struct filesystem *f, const char *dir, int i)
{
    struct lro *L = self(f);
    struct lfs_dirent de;
    if (nth_ent(L, dir, i, &de) < 0) return "";
    int k = 0;
    for (; k < LFS_NAME_MAX && de.name[k]; k++) L->namebuf[k] = de.name[k];
    L->namebuf[k] = 0;
    return L->namebuf;
}

static int lr_ent_size_locked(struct filesystem *f, const char *dir, int i)
{
    struct lro *L = self(f);
    struct lfs_dirent de; struct lfs_dinode ino;
    if (nth_ent(L, dir, i, &de) < 0) return 0;
    if (read_inode(L, de.ino, &ino) < 0) return 0;
    return (int)ino.size;
}

static int lr_ent_is_dir_locked(struct filesystem *f, const char *dir, int i)
{
    struct lro *L = self(f);
    struct lfs_dirent de; struct lfs_dinode ino;
    if (nth_ent(L, dir, i, &de) < 0) return 0;
    if (read_inode(L, de.ino, &ino) < 0) return 0;
    return ino.type == LFS_T_DIR;
}


/* --- the lock, in one place ------------------------------------------------
 * Same discipline as c/fs/logitfs.c: every entry point is a wrapper that takes
 * the MOUNT's lock, calls the identically-named `_locked` body and releases it.
 * The bodies are unchanged. lr_read_locked calls lr_pread_locked directly and
 * not the wrapper, which is what stops the one nesting this file could have.
 *
 * irqsave, for logitfs.c's reason: the holder must not be preempted, and
 * blk_dev_read() is a submit-and-poll that runs with the block layer's
 * non-preemption flag raised, so the sti window inside it is already covered.
 *
 * ent_name() hands out a pointer into L->namebuf and carries logitfs.c's
 * hazard 1 unchanged -- the fill is atomic, the lifetime is the VFS ABI's. */
#define LRO_OP(rettype, name, params, args)                     \
    static rettype name params                                  \
    {                                                           \
        struct lro *L_ = self(f);                               \
        uint64_t fl = spin_lock_irqsave(&L_->lock);             \
        rettype r_ = name##_locked args;                        \
        spin_unlock_irqrestore(&L_->lock, fl);                  \
        return r_;                                              \
    }

LRO_OP(int, lr_mount, (struct filesystem *f), (f))
LRO_OP(int, lr_size, (struct filesystem *f, const char *path), (f, path))
LRO_OP(int, lr_read, (struct filesystem *f, const char *path, void *buf, int max), (f, path, buf, max))
LRO_OP(int, lr_pread, (struct filesystem *f, const char *path, void *buf, int max, long long off),
       (f, path, buf, max, off))
LRO_OP(int, lr_count, (struct filesystem *f, const char *dir), (f, dir))
LRO_OP(const char *, lr_ent_name, (struct filesystem *f, const char *dir, int i), (f, dir, i))
LRO_OP(int, lr_ent_size, (struct filesystem *f, const char *dir, int i), (f, dir, i))
LRO_OP(int, lr_ent_is_dir, (struct filesystem *f, const char *dir, int i), (f, dir, i))

/* No write/del/mkdir/rename: read-only, and a NULL op is a clean -1 through
 * the VFS dispatch rather than a half-written block. */
static const struct fs_iops lfsro_iops = {
    lr_mount, lr_umount, NULL,
    lr_size, lr_read, lr_count, lr_ent_name, lr_ent_size, lr_ent_is_dir,
    NULL, NULL, NULL, NULL,
    lr_pread,                    /* positional: pread is LAST in struct fs_iops */
};

struct filesystem *lfsro_create(const char *dev)
{
    struct blkdev *d = blk_find(dev);
    if (!d) { kprintf("[lfsro] no block device '%s'\n", dev ? dev : "(null)"); return NULL; }
    /* THE SCAN AND THE CLAIM ARE ONE CRITICAL SECTION. Splitting them is the
     * bug: two callers both read used==0 for slot i and both take it, and the
     * loser's mount silently rewrites the winner's device pointer. The rest of
     * the initialisation is under the lock too because it is bounded (a name
     * copy and a dozen stores) and because publishing `used` before `priv` is
     * the half-built-object shape the WM's window table is documented to have. */
    uint64_t pf = spin_lock_irqsave(&pool_lock);
    for (int i = 0; i < LFSRO_MAXFS; i++) {
        if (pool[i].used) continue;
        struct lro *L = &pool[i];
        L->used = 1;
        L->dev = d;
        /* Not SPINLOCK_INIT-by-static: a slot is reused after lfsro_destroy, and
         * a lock left with owner_cpu from its last holder makes spin_unlock's
         * bad-release check report a phantom. ticket == serving == 0 is free. */
        L->lock = (spinlock_t)SPINLOCK_INIT;
        int k = 0;
        for (; k < (int)sizeof L->name - 1 && dev[k]; k++) L->name[k] = dev[k];
        L->name[k] = 0;
        L->fs.mount = NULL; L->fs.list = NULL; L->fs.size = NULL; L->fs.read = NULL;
        L->fs.count = NULL; L->fs.ent_name = NULL; L->fs.ent_size = NULL;
        L->fs.ent_is_dir = NULL; L->fs.write = NULL; L->fs.del = NULL;
        L->fs.mkdir = NULL; L->fs.rename = NULL;
        L->fs.getattr = NULL; L->fs.setattr = NULL; L->fs.umount = NULL;
        L->fs.name = L->name;
        L->fs.iops = &lfsro_iops;
        L->fs.priv = L;
        spin_unlock_irqrestore(&pool_lock, pf);
        return &L->fs;
    }
    spin_unlock_irqrestore(&pool_lock, pf);
    return NULL;
}

void lfsro_destroy(struct filesystem *fs)
{
    if (!fs || !fs->priv) return;
    /* Releasing the slot is a write to the same table the scan above reads.
     * It does NOT take the instance's own lock: an operation in flight on this
     * mount holds that, and taking it here would make destroy WAIT for a
     * caller that is about to dereference a filesystem the VFS has already
     * unmounted -- the lifetime is the VFS's to enforce (it removes the mount
     * before calling this), not something a lock in this file can rescue. */
    uint64_t pf = spin_lock_irqsave(&pool_lock);
    ((struct lro *)fs->priv)->used = 0;
    spin_unlock_irqrestore(&pool_lock, pf);
}
