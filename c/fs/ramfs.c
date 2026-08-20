/* An in-memory filesystem. See ramfs.h for why it exists.
 *
 * Deliberately allocation-free: instances and file bytes come out of a static
 * pool rather than kmalloc. That is not frugality, it is so this file compiles
 * unchanged into the host unit tests (no kheap, no kernel) AND can be mounted
 * from a context where the heap is not a good idea. The cost is a fixed
 * capacity and a bump allocator that does not reclaim a rewritten file's old
 * bytes until the whole instance is dropped -- stated here rather than
 * discovered later, and the reason this is /tmp and not /home. */

#include <stddef.h>
#include "ramfs.h"
#include "vfs_path.h"

#define RAMFS_MAXFS   4
#define RAMFS_NENT   48
#define RAMFS_PATH   96
#define RAMFS_ARENA  (16 * 1024)

struct rent {
    char path[RAMFS_PATH];      /* "" = free; canonical, fs-relative, "/" for root */
    int  is_dir;
    int  off, size;             /* slice of the instance arena */
};

struct rinst {
    int  used;
    char label[16];
    struct filesystem fs;
    struct rent e[RAMFS_NENT];
    char arena[RAMFS_ARENA];
    int  brk;
    char namebuf[VFS_NAME_MAX + 2];
};

static struct rinst pool[RAMFS_MAXFS];

static int  r_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int  r_eq(const char *a, const char *b)
{ int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }
static void r_cpy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void r_mov(char *d, const char *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

static struct rinst *self(struct filesystem *f) { return (struct rinst *)f->priv; }

static struct rent *ent_find(struct rinst *R, const char *path)
{
    for (int i = 0; i < RAMFS_NENT; i++)
        if (R->e[i].path[0] && r_eq(R->e[i].path, path)) return &R->e[i];
    return NULL;
}

static struct rent *ent_new(struct rinst *R, const char *path, int is_dir)
{
    if (r_len(path) + 1 > RAMFS_PATH) return NULL;
    for (int i = 0; i < RAMFS_NENT; i++) {
        if (R->e[i].path[0]) continue;
        r_cpy(R->e[i].path, path, RAMFS_PATH);
        R->e[i].is_dir = is_dir;
        R->e[i].off = 0; R->e[i].size = 0;
        return &R->e[i];
    }
    return NULL;
}

/* Is `path` an immediate child of `dir`? "/" is the parent of "/x" but not of
 * "/x/y" -- the same component-boundary rule the mount table uses. */
static int is_child(const char *dir, const char *path)
{
    int dl = r_len(dir);
    if (dl == 1) {                                   /* dir == "/" */
        if (path[0] != '/' || !path[1]) return 0;
        for (int i = 1; path[i]; i++) if (path[i] == '/') return 0;
        return 1;
    }
    for (int i = 0; i < dl; i++) if (path[i] != dir[i]) return 0;
    if (path[dl] != '/' || !path[dl + 1]) return 0;
    for (int i = dl + 1; path[i]; i++) if (path[i] == '/') return 0;
    return 1;
}

static const char *leaf(const char *path)
{
    const char *s = path;
    for (const char *p = path; *p; p++) if (*p == '/') s = p + 1;
    return s;
}

/* --- ops ---------------------------------------------------------------- */

static int rf_mount(struct filesystem *f) { (void)f; return 0; }

static void rf_umount(struct filesystem *f)
{
    struct rinst *R = self(f);
    for (int i = 0; i < RAMFS_NENT; i++) R->e[i].path[0] = 0;
    R->brk = 0;
    r_cpy(R->e[0].path, "/", RAMFS_PATH);
    R->e[0].is_dir = 1;
}

static int rf_size(struct filesystem *f, const char *path)
{
    struct rent *e = ent_find(self(f), path);
    return (e && !e->is_dir) ? e->size : -1;
}

/* `max` bytes from byte `off`; short at end of file, 0 at or past it.
 *
 * A ramfs entry is a slice of one contiguous arena, so this is a bounds check
 * and a copy -- which is the point of implementing it here rather than letting
 * the VFS refuse: leaving ->pread NULL would make the second mount the one
 * place on the machine where a partial read is impossible, and the failure
 * would surface far from here (a fault that cannot be filled, a stream that
 * returns -1 on its first refill). */
static int rf_pread(struct filesystem *f, const char *path, void *buf, int max, long long off)
{
    struct rinst *R = self(f);
    struct rent *e = ent_find(R, path);
    if (!e || e->is_dir || max < 0 || off < 0) return -1;
    if (off >= (long long)e->size) return 0;
    int n = e->size - (int)off;
    if (n > max) n = max;
    if (n > 0) r_mov((char *)buf, R->arena + e->off + (int)off, n);
    return n;
}

/* Whole-file, and note that ramfs has ALWAYS truncated rather than refused --
 * unlike logitfs, whose ->read is all-or-nothing. That difference is older than
 * this file's involvement and is left exactly as it was; vfs_pread is the entry
 * point that behaves the same on every backend. */
static int rf_read(struct filesystem *f, const char *path, void *buf, int max)
{
    struct rinst *R = self(f);
    struct rent *e = ent_find(R, path);
    if (!e || e->is_dir) return -1;
    int n = e->size < max ? e->size : max;
    if (n > 0) r_mov((char *)buf, R->arena + e->off, n);
    return n;
}

static int rf_write(struct filesystem *f, const char *path, const void *buf, int size)
{
    struct rinst *R = self(f);
    if (size < 0) return -1;
    struct rent *e = ent_find(R, path);
    if (e && e->is_dir) return -1;
    if (!e) {
        /* The parent has to exist, or a ramfs quietly becomes a flat namespace
         * where "/a/b" is a file whose name contains a slash. */
        char dir[RAMFS_PATH];
        if (vfs_path_split(path, dir, (int)sizeof dir, NULL, 0) < 0) return -1;
        struct rent *p = ent_find(R, dir);
        if (!p || !p->is_dir) return -1;
        e = ent_new(R, path, 0);
        if (!e) return -1;
    }
    if (size > e->size || e->off == 0) {         /* needs a (bigger) slice */
        if (R->brk + size > RAMFS_ARENA) return -1;
        e->off = R->brk;
        R->brk += size ? size : 1;
    }
    if (size > 0) r_mov(R->arena + e->off, (const char *)buf, size);
    e->size = size;
    return size;
}

static int rf_count(struct filesystem *f, const char *dir)
{
    struct rinst *R = self(f);
    struct rent *d = ent_find(R, dir);
    if (!d || !d->is_dir) return -1;
    int n = 0;
    for (int i = 0; i < RAMFS_NENT; i++)
        if (R->e[i].path[0] && is_child(dir, R->e[i].path)) n++;
    return n;
}

static struct rent *nth(struct rinst *R, const char *dir, int idx)
{
    int n = 0;
    for (int i = 0; i < RAMFS_NENT; i++) {
        if (!R->e[i].path[0] || !is_child(dir, R->e[i].path)) continue;
        if (n++ == idx) return &R->e[i];
    }
    return NULL;
}

static const char *rf_ent_name(struct filesystem *f, const char *dir, int i)
{
    struct rinst *R = self(f);
    struct rent *e = nth(R, dir, i);
    if (!e) return "";
    r_cpy(R->namebuf, leaf(e->path), (int)sizeof R->namebuf);
    return R->namebuf;
}

static int rf_ent_size(struct filesystem *f, const char *dir, int i)
{ struct rent *e = nth(self(f), dir, i); return e ? e->size : 0; }

static int rf_ent_is_dir(struct filesystem *f, const char *dir, int i)
{ struct rent *e = nth(self(f), dir, i); return e ? e->is_dir : 0; }

static int rf_mkdir(struct filesystem *f, const char *path)
{
    struct rinst *R = self(f);
    if (ent_find(R, path)) return -1;
    char dir[RAMFS_PATH];
    if (vfs_path_split(path, dir, (int)sizeof dir, NULL, 0) < 0) return -1;
    struct rent *p = ent_find(R, dir);
    if (!p || !p->is_dir) return -1;
    return ent_new(R, path, 1) ? 0 : -1;
}

static int rf_del(struct filesystem *f, const char *path)
{
    struct rinst *R = self(f);
    struct rent *e = ent_find(R, path);
    if (!e) return -1;
    if (e->is_dir && rf_count(f, path) > 0) return VFS_ENOTEMPTY;
    e->path[0] = 0;
    return 0;
}

static int rf_rename(struct filesystem *f, const char *o, const char *n)
{
    struct rinst *R = self(f);
    struct rent *e = ent_find(R, o);
    if (!e) return -1;
    if (ent_find(R, n)) return -1;
    if (r_len(n) + 1 > RAMFS_PATH) return -1;
    char dir[RAMFS_PATH];
    if (vfs_path_split(n, dir, (int)sizeof dir, NULL, 0) < 0) return -1;
    struct rent *p = ent_find(R, dir);
    if (!p || !p->is_dir) return -1;
    r_cpy(e->path, n, RAMFS_PATH);
    return 0;
}

static const struct fs_iops ramfs_iops = {
    rf_mount, rf_umount, NULL,
    rf_size, rf_read, rf_count, rf_ent_name, rf_ent_size, rf_ent_is_dir,
    rf_write, rf_del, rf_mkdir, rf_rename,
    rf_pread,                    /* positional: pread is LAST in struct fs_iops */
};

struct filesystem *ramfs_create(const char *label)
{
    for (int i = 0; i < RAMFS_MAXFS; i++) {
        if (pool[i].used) continue;
        struct rinst *R = &pool[i];
        R->used = 1;
        r_cpy(R->label, label && label[0] ? label : "ramfs", (int)sizeof R->label);
        for (int k = 0; k < RAMFS_NENT; k++) R->e[k].path[0] = 0;
        R->brk = 0;
        r_cpy(R->e[0].path, "/", RAMFS_PATH);
        R->e[0].is_dir = 1;
        /* Clear the singleton ops explicitly: a recycled slot must not keep a
         * previous life's function pointers, and `iops` being present means
         * none of them should ever be consulted anyway. */
        R->fs.mount = NULL; R->fs.list = NULL; R->fs.size = NULL; R->fs.read = NULL;
        R->fs.count = NULL; R->fs.ent_name = NULL; R->fs.ent_size = NULL;
        R->fs.ent_is_dir = NULL; R->fs.write = NULL; R->fs.del = NULL;
        R->fs.mkdir = NULL; R->fs.rename = NULL;
        R->fs.getattr = NULL; R->fs.setattr = NULL; R->fs.umount = NULL;
        R->fs.name = R->label;
        R->fs.iops = &ramfs_iops;
        R->fs.priv = R;
        return &R->fs;
    }
    return NULL;
}

void ramfs_destroy(struct filesystem *fs)
{
    if (!fs || !fs->priv) return;
    struct rinst *R = (struct rinst *)fs->priv;
    R->used = 0;
}
