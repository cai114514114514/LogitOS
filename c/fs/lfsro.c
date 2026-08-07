/* A read-only LogitFS reader, one instance per block device. See lfsro.h for
 * why this is a second reader of a format that already has one. */

#include <stdint.h>
#include <stddef.h>
#include "lfsro.h"
#include "logitfs_fmt.h"
#include "blkdev.h"
#include "vfs_path.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

#define LFSRO_MAXFS 2

struct lro {
    int   used;
    char  name[24];
    struct blkdev *dev;
    struct lfs_super sb;
    struct filesystem fs;
    uint8_t  blk[LFS_BS];        /* one block of staging; every read is synchronous
                                  * and under the BKL, so one buffer is enough */
    uint8_t  dblk[LFS_BS];       /* a second, for directory scans that also need
                                  * to fetch an inode mid-walk */
    uint32_t ind[LFS_PPB];
    char  namebuf[LFS_NAME_MAX + 2];
};

static struct lro pool[LFSRO_MAXFS];

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

static int lr_mount(struct filesystem *f)
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

static void lr_umount(struct filesystem *f) { (void)f; }

static int lr_size(struct filesystem *f, const char *path)
{
    struct lro *L = self(f);
    struct lfs_dinode ino;
    if (path_ino(L, path, &ino) == LFSRO_NOINO) return -1;
    return ino.type == LFS_T_FILE ? (int)ino.size : -1;
}

static int lr_read(struct filesystem *f, const char *path, void *buf, int max)
{
    struct lro *L = self(f);
    struct lfs_dinode ino;
    if (path_ino(L, path, &ino) == LFSRO_NOINO || ino.type != LFS_T_FILE) return -1;
    int want = (int)ino.size < max ? (int)ino.size : max;
    int done = 0;
    while (done < want) {
        uint32_t phys = bmap(L, &ino, (uint32_t)(done / LFS_BS));
        int off = done % LFS_BS;
        int n = LFS_BS - off;
        if (n > want - done) n = want - done;
        if (!phys || rd(L, phys, L->blk) < 0) return done ? done : -1;
        memcpy((char *)buf + done, L->blk + off, (size_t)n);
        done += n;
    }
    return done;
}

static int lr_count(struct filesystem *f, const char *dir)
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

static const char *lr_ent_name(struct filesystem *f, const char *dir, int i)
{
    struct lro *L = self(f);
    struct lfs_dirent de;
    if (nth_ent(L, dir, i, &de) < 0) return "";
    int k = 0;
    for (; k < LFS_NAME_MAX && de.name[k]; k++) L->namebuf[k] = de.name[k];
    L->namebuf[k] = 0;
    return L->namebuf;
}

static int lr_ent_size(struct filesystem *f, const char *dir, int i)
{
    struct lro *L = self(f);
    struct lfs_dirent de; struct lfs_dinode ino;
    if (nth_ent(L, dir, i, &de) < 0) return 0;
    if (read_inode(L, de.ino, &ino) < 0) return 0;
    return (int)ino.size;
}

static int lr_ent_is_dir(struct filesystem *f, const char *dir, int i)
{
    struct lro *L = self(f);
    struct lfs_dirent de; struct lfs_dinode ino;
    if (nth_ent(L, dir, i, &de) < 0) return 0;
    if (read_inode(L, de.ino, &ino) < 0) return 0;
    return ino.type == LFS_T_DIR;
}

/* No write/del/mkdir/rename: read-only, and a NULL op is a clean -1 through
 * the VFS dispatch rather than a half-written block. */
static const struct fs_iops lfsro_iops = {
    lr_mount, lr_umount, NULL,
    lr_size, lr_read, lr_count, lr_ent_name, lr_ent_size, lr_ent_is_dir,
    NULL, NULL, NULL, NULL,
};

struct filesystem *lfsro_create(const char *dev)
{
    struct blkdev *d = blk_find(dev);
    if (!d) { kprintf("[lfsro] no block device '%s'\n", dev ? dev : "(null)"); return NULL; }
    for (int i = 0; i < LFSRO_MAXFS; i++) {
        if (pool[i].used) continue;
        struct lro *L = &pool[i];
        L->used = 1;
        L->dev = d;
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
        return &L->fs;
    }
    return NULL;
}

void lfsro_destroy(struct filesystem *fs)
{
    if (!fs || !fs->priv) return;
    ((struct lro *)fs->priv)->used = 0;
}
