#ifndef LOGIT_VFS_H
#define LOGIT_VFS_H

#include "vfs_path.h"    /* VFS_E* and the path walker */
#include "vfs_meta.h"    /* struct vattr / struct vcred / MAY_* */

/* ---------------------------------------------------------------------------
 * The virtual filesystem layer.
 *
 * This used to be 86 lines that forwarded every call to a single registered
 * backend. It now does the four things that make a VFS a layer rather than a
 * forwarding shim:
 *
 *   1. A MOUNT TABLE. Several filesystems on several devices, resolved during
 *      the path walk by longest matching prefix. The root is mounted, not
 *      assumed -- vfs_register()+vfs_mount() is still the way the root arrives,
 *      but it now installs a mount at "/" like any other.
 *   2. PERMISSIONS. uid/gid/mode checked against the calling process's
 *      credential on open, read, write, unlink and directory search. Enforced,
 *      not merely stored: see vfs_meta.h on why that distinction is the whole
 *      point.
 *   3. LINKS. Hard links with a real link count and correct unlink semantics,
 *      and symbolic links expanded during resolution with a bounded chain.
 *   4. A REAL PATH WALK. "." and "..", mount crossings, symlink expansion, and
 *      a defined answer for a path that tries to escape the root (it does not).
 *      Implemented in vfs_path.c, which has no kernel dependencies precisely so
 *      that the cases where the bugs are can be unit-tested on the host.
 *
 * Backends stay path-addressed and unchanged. logitfs did not have to be
 * touched to get any of this, which was a requirement rather than a
 * preference: it is being rewritten underneath by the crash-consistency line.
 * ------------------------------------------------------------------------ */

/* A filesystem driver. The first block is the original, unchanged interface --
 * every existing backend still compiles and behaves identically. The ops after
 * it are OPTIONAL: a NULL means "the VFS handles this on your behalf", which
 * is how metadata and links work today. A backend that grows real on-disk
 * owner/mode fields fills in getattr/setattr and takes over, with no change
 * anywhere else. */
struct filesystem;

/* The instance-aware form of the operations below.
 *
 * The original ops take no `self`, which is exactly what makes logitfs a
 * singleton: its superblock, bitmap and inode table are file-static, and there
 * is no argument that could say WHICH mount is being asked. Widening those
 * signatures would mean editing c/fs/logitfs.c, which is being rewritten by
 * another line right now, so instead a backend that can be mounted more than
 * once -- a ramfs per mount point, a read-only LogitFS per block device --
 * publishes this vtable and the VFS prefers it. A backend fills in exactly one
 * of the two; logitfs keeps the one it has and is untouched. */
struct fs_iops {
    int  (*mount)(struct filesystem *);
    void (*umount)(struct filesystem *);
    void (*list)(struct filesystem *);
    int  (*size)(struct filesystem *, const char *path);
    int  (*read)(struct filesystem *, const char *path, void *buf, int max);
    int  (*count)(struct filesystem *, const char *dir);
    const char *(*ent_name)(struct filesystem *, const char *dir, int i);
    int  (*ent_size)(struct filesystem *, const char *dir, int i);
    int  (*ent_is_dir)(struct filesystem *, const char *dir, int i);
    int  (*write)(struct filesystem *, const char *path, const void *buf, int size);
    int  (*del)(struct filesystem *, const char *path);
    int  (*mkdir)(struct filesystem *, const char *path);
    int  (*rename)(struct filesystem *, const char *o, const char *n);
    /* Appended at the END on purpose: ramfs_iops and lfsro_iops are POSITIONAL
     * initializers, so a member inserted anywhere else silently re-aims every
     * pointer after it. See ->pread on struct filesystem for what it is. */
    int  (*pread)(struct filesystem *, const char *path, void *buf, int max, long long off);
};

struct filesystem {
    const char *name;
    int  (*mount)(void);
    void (*list)(void);
    int  (*size)(const char *path);                 /* bytes, or -1 */
    int  (*read)(const char *path, void *buf, int max);  /* bytes read, or -1 */
    int  (*count)(const char *dir);                 /* entries in directory `dir` */
    const char *(*ent_name)(const char *dir, int i);/* name of entry i in `dir` */
    int  (*ent_size)(const char *dir, int i);        /* size of entry i, bytes */
    int  (*ent_is_dir)(const char *dir, int i);      /* 1 if entry i is a directory */
    int  (*write)(const char *path, const void *buf, int size);  /* create/overwrite */
    int  (*del)(const char *path);                  /* delete */
    int  (*mkdir)(const char *path);                /* create a directory */
    int  (*rename)(const char *old, const char *new_path); /* re-link a dir entry (move/rename) */

    /* --- optional (may be NULL) --- */
    /* Per-file owner/mode straight off the medium. Implementing BOTH takes the
     * file out of the VFS metadata store; implementing neither leaves it in.
     * Implementing one is a bug and is treated as implementing neither. */
    int  (*getattr)(const char *path, struct vattr *a);
    int  (*setattr)(const char *path, const struct vattr *a);
    /* Read `max` bytes of `path` starting at byte `off`. Returns the count
     * actually read -- SHORT at end of file, 0 at or past it -- or -1.
     *
     * A SEPARATE op and not a widening of ->read, because ->read already means
     * something that several callers depend on: on logitfs it is ALL OR
     * NOTHING (`if (size > (uint32_t)max) return -1` -- a request that does not
     * cover the whole file is REFUSED, not truncated), and twenty callers pass
     * a buffer sized from vfs_size and treat any short return as failure.
     * Changing what ->read means is a flag day; adding what it never had is
     * not.
     *
     * A backend that leaves this NULL is one whose files can only be read
     * whole, and vfs_pread refuses out loud (VFS_ENOSYS) rather than emulating
     * it by slurping the file and throwing the prefix away. That emulation is
     * O(off) per call, so a sequential walk of an n-page file costs O(n^2)
     * bytes of copying -- it is exactly the workaround that lived in
     * c/kernel/mm/pcache_vfs.c until this op existed, and it is deleted rather
     * than kept as a second path that can disagree with this one. */
    int  (*pread)(const char *path, void *buf, int max, long long off);

    /* Unmount hook: flush and release. */
    void (*umount)(void);
    /* Present => this backend is instance-aware and `iops` is used instead of
     * every op above except getattr/setattr. */
    const struct fs_iops *iops;
    void *priv;                                     /* backend instance cookie */
};

/* --- mounting ----------------------------------------------------------- */

/* Back-compatible root registration: remembers `fs` as the root filesystem.
 * vfs_mount() then mounts it at "/". */
void vfs_register(struct filesystem *fs);
int  vfs_mount(void);

/* Mount `fs` at the (already existing) directory `dir`. `dir` must be
 * canonical and absolute. Calls fs->mount() first; a backend that fails to
 * mount is not installed. */
int  vfs_mount_at(const char *dir, struct filesystem *fs);
int  vfs_umount(const char *dir);
int  vfs_mount_count(void);
/* Render the mount table, /proc/mounts style. Returns bytes written. */
int  vfs_mounts_render(char *buf, int max);

void vfs_list(void);

/* --- the path-addressed operations (unchanged signatures) ---------------- */
int  vfs_size(const char *path);
int  vfs_read(const char *path, void *buf, int max);
/* read(2)'s shape, at last: `max` bytes from byte `off`, short at EOF, 0 at or
 * past it. Every other operation in this header is whole-file, which is why the
 * kernel could map a file before anything above it could ask for part of one.
 * Negative return = a VFS_E*; VFS_ENOSYS = this backend cannot read part of a
 * file (see ->pread on struct filesystem). */
int  vfs_pread(const char *path, void *buf, int max, long long off);
int  vfs_count(const char *dir);
const char *vfs_ent_name(const char *dir, int i);
int  vfs_ent_size(const char *dir, int i);
int  vfs_ent_is_dir(const char *dir, int i);
int  vfs_write(const char *path, const void *buf, int size);
int  vfs_delete(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_rename(const char *old_path, const char *new_path);

/* --- what the layer gained ---------------------------------------------- */

/* Would the CURRENT credential be allowed `want` (a mask of MAY_*) on `path`?
 * 0 = yes, a negative VFS_E* = no. This is the call the fd layer makes at
 * open() and the one an exec path should make before loading an image. */
int  vfs_access(const char *path, int want);

/* May the current credential create a name at `path` that does not exist yet?
 * A separate question from vfs_access, and the one open(O_CREAT) has to ask:
 * there is no file to check the mode of, so the answer comes from the
 * directory that would gain the entry. */
int  vfs_may_create(const char *path);

/* Attributes of `path` (follows symlinks). */
int  vfs_stat(const char *path, struct vattr *a);
int  vfs_lstat(const char *path, struct vattr *a);

/* The same, plus size / blocks / inode / device / timestamps, and the VA_* bits
 * saying which of them are real. vfs_stat answers only the permission question
 * -- everything above it that wants to REPORT a file needs this one. `follow`
 * picks stat vs lstat semantics. */
int  vfs_statx(const char *path, struct vattr *a, int follow);

/* One directory entry as getdents reports it. The name is 256 bytes because the
 * ABI's is; what an individual backend can actually store is smaller (LogitFS
 * dirents cap a name at 59 bytes + NUL), and a name that does not fit is
 * rejected by the backend at create time rather than truncated here. */
struct vdirent {
    char     name[256];
    int      type;              /* VT_REG / VT_DIR / VT_LNK */
    uint64_t ino;               /* 0 when the backend has no inode numbers */
    uint64_t size;
};

/* Fill up to `max` entries starting at *cursor, advancing it. Returns the count
 * written (0 = end of directory) or a negative VFS_E*. The cursor is opaque to
 * the caller; see struct logit_dirreq in include/abi/logit_abi.h for exactly
 * what it does and does not promise across a concurrent modification. */
int  vfs_getdents(const char *dir, int *cursor, struct vdirent *out, int max);

/* The calling process's file-creation mask. `set` < 0 queries. Returns the
 * PREVIOUS mask, which is what makes the save/restore idiom possible. */
unsigned int vfs_umask(int set);

/* The pid the credential and mask lookups key on. Implemented in vfs_cred.c;
 * referenced WEAKLY from vfs.c so the host unit tests need not supply it. */
int  vfs_cred_pid(void);

int  vfs_chmod(const char *path, unsigned mode);
int  vfs_chown(const char *path, unsigned uid, unsigned gid);

int  vfs_link(const char *oldpath, const char *newpath);     /* hard link */
int  vfs_symlink(const char *target, const char *linkpath);
int  vfs_readlink(const char *path, char *buf, int max);     /* bytes, or VFS_E* */
int  vfs_nlink(const char *path);

/* Resolve `in` against `cwd` exactly as the operations above do -- symlinks
 * expanded, ".." applied to resolved ground, mount points crossed. Callers
 * that need the canonical name of something (the fd layer, a shell's cd) use
 * this rather than re-implementing a walk. */
int  vfs_resolve(const char *cwd, const char *in, char *out, int max, int follow_final);

/* --- process credentials ------------------------------------------------ */
/* Implemented in vfs_cred.c against the process table. Declared here because
 * the enforcement points are in this layer; the host unit tests supply their
 * own definition to drive the checks without a kernel. */
void vfs_cred_current(struct vcred *c);

#endif /* LOGIT_VFS_H */
