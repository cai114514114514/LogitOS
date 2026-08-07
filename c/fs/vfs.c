/* The virtual filesystem layer. See vfs.h for what this became and why.
 *
 * No kernel headers beyond the two synthetic-file providers, both of which are
 * plain declarations -- so this file, the one that makes every permission
 * decision, compiles into the host unit tests with those providers stubbed.
 * The enforcement code under test is the enforcement code that ships. */

#include <stddef.h>
#include "vfs.h"
#include "vfs_path.h"
#include "vfs_meta.h"
#include "kdiag.h"      /* kernel-served synthetic files: /dev/kmsg, kstat, ktrigger */
#include "vfsctl.h"     /* the VFS's own control + introspection nodes under /dev */

/* Synthetic files come first. They are served by the kernel itself (no disk, no
 * inode) and every one of them returns KDIAG_NOT_MINE for a path it does not
 * own, so an unrelated path falls through to the mounted filesystem unchanged.
 * This is the whole cost of making the kernel log readable from userland:
 * `cat /dev/kmsg` needs no new syscall and no new ABI number. The VFS's own
 * nodes (/dev/vfsctl, /dev/vfsmounts, /dev/vfsmeta) follow the same protocol
 * for the same reason -- see vfsctl.h. */

/* --------------------------------------------------------------------------
 * The mount table
 * ------------------------------------------------------------------------ */

#define VFS_NMOUNT 8
#define VFS_MPOINT_MAX 64

struct vmountent {
    char  at[VFS_MPOINT_MAX];      /* canonical mount point, "" = free */
    struct filesystem *fs;
};

static struct vmountent mounts[VFS_NMOUNT];
static struct filesystem *pending_root;    /* what vfs_register() was handed */

/* --- one dispatch point per operation -------------------------------------
 * Every call into a backend goes through these, so the choice between the
 * singleton ops and the instance-aware vtable is made in exactly one place per
 * operation instead of at fifteen call sites. See struct fs_iops in vfs.h for
 * why there are two forms at all. */
static int fs_mount(struct filesystem *f)
{ return f->iops ? (f->iops->mount ? f->iops->mount(f) : 0) : (f->mount ? f->mount() : 0); }
static void fs_umount(struct filesystem *f)
{ if (f->iops) { if (f->iops->umount) f->iops->umount(f); } else if (f->umount) f->umount(); }
static void fs_list(struct filesystem *f)
{ if (f->iops) { if (f->iops->list) f->iops->list(f); } else if (f->list) f->list(); }
static int fs_size(struct filesystem *f, const char *p)
{ return f->iops ? (f->iops->size ? f->iops->size(f, p) : -1) : (f->size ? f->size(p) : -1); }
static int fs_read(struct filesystem *f, const char *p, void *b, int m)
{ return f->iops ? (f->iops->read ? f->iops->read(f, p, b, m) : -1) : (f->read ? f->read(p, b, m) : -1); }
static int fs_count(struct filesystem *f, const char *d)
{ return f->iops ? (f->iops->count ? f->iops->count(f, d) : -1) : (f->count ? f->count(d) : -1); }
static const char *fs_ent_name(struct filesystem *f, const char *d, int i)
{ return f->iops ? (f->iops->ent_name ? f->iops->ent_name(f, d, i) : "") : (f->ent_name ? f->ent_name(d, i) : ""); }
static int fs_ent_size(struct filesystem *f, const char *d, int i)
{ return f->iops ? (f->iops->ent_size ? f->iops->ent_size(f, d, i) : 0) : (f->ent_size ? f->ent_size(d, i) : 0); }
static int fs_ent_is_dir(struct filesystem *f, const char *d, int i)
{ return f->iops ? (f->iops->ent_is_dir ? f->iops->ent_is_dir(f, d, i) : 0) : (f->ent_is_dir ? f->ent_is_dir(d, i) : 0); }
static int fs_write(struct filesystem *f, const char *p, const void *b, int n)
{ return f->iops ? (f->iops->write ? f->iops->write(f, p, b, n) : -1) : (f->write ? f->write(p, b, n) : -1); }
static int fs_del(struct filesystem *f, const char *p)
{ return f->iops ? (f->iops->del ? f->iops->del(f, p) : -1) : (f->del ? f->del(p) : -1); }
static int fs_mkdir(struct filesystem *f, const char *p)
{ return f->iops ? (f->iops->mkdir ? f->iops->mkdir(f, p) : -1) : (f->mkdir ? f->mkdir(p) : -1); }
static int fs_rename(struct filesystem *f, const char *o, const char *n)
{ return f->iops ? (f->iops->rename ? f->iops->rename(f, o, n) : VFS_ENOSYS)
                 : (f->rename ? f->rename(o, n) : VFS_ENOSYS); }

static int  s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int  s_eq(const char *a, const char *b)
{ int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }
static void s_cpy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* Longest-prefix match. A path always belongs to exactly one mount: the
 * deepest mount point that is a component-boundary prefix of it. That is what
 * makes a mount at /mnt shadow the /mnt of the filesystem underneath rather
 * than merging with it, and what makes "/mnttab" keep belonging to the root. */
static struct vmountent *mount_for(const char *abs)
{
    struct vmountent *best = NULL;
    int bestlen = -1;
    for (int i = 0; i < VFS_NMOUNT; i++) {
        if (!mounts[i].at[0] || !mounts[i].fs) continue;
        if (!vfs_path_is_prefix(mounts[i].at, abs)) continue;
        int l = s_len(mounts[i].at);
        if (l > bestlen) { bestlen = l; best = &mounts[i]; }
    }
    return best;
}

/* The path AS THE BACKEND SEES IT: global path minus the mount point, so a
 * filesystem mounted at /mnt is asked for "/hello.txt", not "/mnt/hello.txt".
 * A backend therefore needs no idea that it is not the root, which is the
 * property that let a second filesystem be mounted without touching one. */
static const char *sub_path(const struct vmountent *m, const char *abs, char *buf, int max)
{
    int l = s_len(m->at);
    if (l == 1) return abs;                     /* mounted at "/" */
    const char *rest = abs + l;
    if (!rest[0]) return "/";
    s_cpy(buf, rest, max);
    return buf;
}

/* Is `abs` a mount point of some OTHER filesystem? Used to refuse operations
 * that would corrupt the table (unlinking a mount point, renaming across it). */
static int is_mount_point(const char *abs)
{
    for (int i = 0; i < VFS_NMOUNT; i++)
        if (mounts[i].at[0] && mounts[i].fs && s_eq(mounts[i].at, abs) && !s_eq(abs, "/"))
            return 1;
    return 0;
}

void vfs_register(struct filesystem *fs) { pending_root = fs; }

int vfs_mount(void)
{
    if (!pending_root) return -1;
    return vfs_mount_at("/", pending_root);
}

int vfs_mount_at(const char *dir, struct filesystem *fs)
{
    if (!fs || !dir || dir[0] != '/') return VFS_EINVAL;
    char at[VFS_MPOINT_MAX];
    if (vfs_path_norm("/", dir, at, (int)sizeof at) < 0) return VFS_ENAMETOOLONG;

    for (int i = 0; i < VFS_NMOUNT; i++)
        if (mounts[i].at[0] && s_eq(mounts[i].at, at)) return VFS_EBUSY;

    /* Everything except "/" must be mounted ON something, and that something
     * has to already be a directory: a mount point that does not exist is a
     * filesystem you can only reach by knowing it is there. */
    if (!s_eq(at, "/")) {
        struct vmountent *par = mount_for(at);
        if (!par) return VFS_ENOENT;
        char sub[VFS_PATH_MAX];
        const char *sp = sub_path(par, at, sub, (int)sizeof sub);
        /* Two different failures, told apart on purpose: a mount point that is
         * not there at all, and one that is there but is a file. Collapsing
         * them into "cannot mount" is how ten minutes go into checking a typo
         * that was actually a missing mkdir. */
        if (fs_count(par->fs, sp) < 0)
            return fs_size(par->fs, sp) >= 0 ? VFS_ENOTDIR : VFS_ENOENT;
    }

    int slot = -1;
    for (int i = 0; i < VFS_NMOUNT; i++) if (!mounts[i].at[0]) { slot = i; break; }
    if (slot < 0) return VFS_ENOSPC;

    if (fs_mount(fs) != 0) return VFS_EIO;   /* a backend that cannot
                                                          * mount is not installed */
    s_cpy(mounts[slot].at, at, VFS_MPOINT_MAX);
    mounts[slot].fs = fs;
    return 0;
}

int vfs_umount(const char *dir)
{
    char at[VFS_MPOINT_MAX];
    if (!dir || vfs_path_norm("/", dir, at, (int)sizeof at) < 0) return VFS_EINVAL;
    for (int i = 0; i < VFS_NMOUNT; i++) {
        if (!mounts[i].at[0] || !s_eq(mounts[i].at, at)) continue;
        /* Refuse while something is mounted underneath: unmounting the middle
         * of the table would leave the deeper filesystem reachable through a
         * path that no longer resolves to it. */
        for (int j = 0; j < VFS_NMOUNT; j++)
            if (j != i && mounts[j].at[0] && vfs_path_is_prefix(at, mounts[j].at))
                return VFS_EBUSY;
        fs_umount(mounts[i].fs);
        /* Metadata records below the mount point describe files that are about
         * to become unreachable; keeping them would apply one filesystem's
         * modes to whatever is mounted there next. */
        vmeta_forget_subtree(at);
        mounts[i].at[0] = 0; mounts[i].fs = NULL;
        return 0;
    }
    return VFS_EINVAL;
}

int vfs_mount_count(void)
{
    int n = 0;
    for (int i = 0; i < VFS_NMOUNT; i++) if (mounts[i].at[0]) n++;
    return n;
}

static int put(char *b, int max, int n, const char *s)
{ for (int i = 0; s && s[i] && n < max - 1; i++) b[n++] = s[i]; return n; }

int vfs_mounts_render(char *buf, int max)
{
    int n = 0;
    for (int i = 0; i < VFS_NMOUNT; i++) {
        if (!mounts[i].at[0]) continue;
        n = put(buf, max, n, mounts[i].fs->name ? mounts[i].fs->name : "?");
        n = put(buf, max, n, " ");
        n = put(buf, max, n, mounts[i].at);
        n = put(buf, max, n, "\n");
    }
    if (n < max) buf[n] = 0;
    return n;
}

void vfs_list(void)
{
    for (int i = 0; i < VFS_NMOUNT; i++)
        if (mounts[i].at[0]) fs_list(mounts[i].fs);
}

/* --------------------------------------------------------------------------
 * Resolution, with the permission check woven into the walk
 *
 * Search permission has to be checked against the directory that is ACTUALLY
 * traversed, which for a path containing symlinks is not the same as the
 * ancestors of the final answer. So the check rides in the walker's per
 * component callback rather than being a second pass over the result.
 * ------------------------------------------------------------------------ */

struct wctx { struct vcred cred; int nocheck; };

static int visit(void *vctx, const char *abs, int is_final, char *link, int max)
{
    struct wctx *w = (struct wctx *)vctx;

    int k = vmeta_readlink(abs, link, max);
    if (k) return k;                     /* 1 = symlink, or a negative error */
    if (is_final || w->nocheck) return 0;

    /* This component is about to be descended into. Only a component with a
     * metadata record can refuse: without one the mode is the default 0755,
     * which grants search to everybody, so skipping the check changes no
     * answer and saves a decision per component. */
    struct vattr a;
    if (!vmeta_lookup(abs, &a)) return 0;
    if (a.type == VT_REG) return VFS_ENOTDIR;
    return vmeta_permission(&a, &w->cred, MAY_EXEC);
}

static void cred_now(struct vcred *c)
{
    c->uid = 0; c->gid = 0;
    vfs_cred_current(c);
}

/* Resolve `in` (already absolute, from the syscall layer) to a canonical path,
 * expanding symlinks and checking search permission on every directory
 * traversed. `follow_final` picks stat vs lstat semantics. */
static int resolve(const char *in, char *out, int max, int follow_final)
{
    struct wctx w;
    cred_now(&w.cred);
    w.nocheck = 0;
    struct vfs_path_env env = { visit, &w, 0 };
    return vfs_path_resolve(&env, "/", in, out, max, follow_final);
}

int vfs_resolve(const char *cwd, const char *in, char *out, int max, int follow_final)
{
    struct wctx w;
    cred_now(&w.cred);
    w.nocheck = 0;
    struct vfs_path_env env = { visit, &w, 0 };
    return vfs_path_resolve(&env, cwd, in, out, max, follow_final);
}

/* --------------------------------------------------------------------------
 * Attributes and the operation-level checks
 * ------------------------------------------------------------------------ */

/* Ask the backend whether `abs` is a directory. Cheap and honest: a filesystem
 * that answers count() >= 0 for a path is saying "this is a directory I can
 * enumerate". Kept in one place so the mode defaults are decided the same way
 * everywhere. */
static int is_dir_on(struct vmountent *m, const char *sub)
{
    if (!m) return 0;
    return fs_count(m->fs, sub) >= 0;
}

static void attrs_of(const char *abs, struct vattr *a)
{
    struct vmountent *m = mount_for(abs);
    char sub[VFS_PATH_MAX];
    int isdir = s_eq(abs, "/") || (m && is_dir_on(m, sub_path(m, abs, sub, (int)sizeof sub)));

    /* A backend that owns its own metadata answers for itself; one that does
     * not gets the VFS store. Both ops must be present -- half an
     * implementation is treated as none, because a getattr without a setattr
     * is a permission model nobody can change. */
    if (m && m->fs->getattr && m->fs->setattr) {
        if (m->fs->getattr(sub_path(m, abs, sub, (int)sizeof sub), a) == 0) return;
    }
    vmeta_attr(abs, isdir, a);
}

/* The permission a caller needs on the CONTAINING DIRECTORY to create, remove
 * or rename a name inside it. This is the check that stops an unprivileged
 * process writing into a root-owned directory, and it applies whether or not
 * the file itself has a record. */
static int check_parent_write(const char *abs, const struct vcred *c)
{
    char dir[VFS_PATH_MAX];
    int rc = vfs_path_split(abs, dir, (int)sizeof dir, NULL, 0);
    if (rc < 0) return rc == VFS_EINVAL ? VFS_EPERM : rc;   /* "/" itself */
    struct vattr a;
    attrs_of(dir, &a);
    return vmeta_permission(&a, c, MAY_WRITE | MAY_EXEC);
}

static int check_file(const char *abs, const struct vcred *c, int want)
{
    struct vattr a;
    attrs_of(abs, &a);
    return vmeta_permission(&a, c, want);
}

int vfs_access(const char *path, int want)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;
    struct vcred c; cred_now(&c);
    return check_file(abs, &c, want);
}

int vfs_may_create(const char *path)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 0);
    if (rc < 0) return rc;
    struct vcred c; cred_now(&c);
    return check_parent_write(abs, &c);
}

int vfs_stat(const char *path, struct vattr *a)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;
    if (vfs_size(abs) < 0 && vfs_count(abs) < 0) return VFS_ENOENT;
    attrs_of(vmeta_canon(abs), a);
    a->nlink = vmeta_nlink(abs);
    return 0;
}

int vfs_lstat(const char *path, struct vattr *a)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 0);
    if (rc < 0) return rc;
    struct vattr la;
    if (vmeta_lookup(abs, &la) && la.type == VT_LNK) { *a = la; return 0; }
    return vfs_stat(abs, a);
}

/* --------------------------------------------------------------------------
 * Synthetic providers. Consulted before any mount, in a fixed order, each one
 * answering NOT_MINE for a path it does not own.
 * ------------------------------------------------------------------------ */

static int syn_size(const char *p)
{
    int n = kdiag_size(p);
    if (n != KDIAG_NOT_MINE) return n;
    return vfsctl_size(p);
}

/* --------------------------------------------------------------------------
 * The operations
 * ------------------------------------------------------------------------ */

int vfs_size(const char *path)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;

    int n = syn_size(abs);
    if (n != KDIAG_NOT_MINE) return n;

    const char *real = vmeta_canon(abs);
    struct vmountent *m = mount_for(real);
    if (!m) return -1;
    char sub[VFS_PATH_MAX];
    return fs_size(m->fs, sub_path(m, real, sub, (int)sizeof sub));
}

int vfs_read(const char *path, void *buf, int max)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;

    struct vcred c; cred_now(&c);

    int n = kdiag_read(abs, buf, max);
    if (n != KDIAG_NOT_MINE) {
        if ((rc = check_file(abs, &c, MAY_READ)) < 0) return rc;
        return n;
    }
    if (vfsctl_size(abs) != KDIAG_NOT_MINE) {
        if ((rc = check_file(abs, &c, MAY_READ)) < 0) return rc;
        return vfsctl_read(abs, buf, max);
    }

    if ((rc = check_file(abs, &c, MAY_READ)) < 0) return rc;

    const char *real = vmeta_canon(abs);
    struct vmountent *m = mount_for(real);
    if (!m) return -1;
    char sub[VFS_PATH_MAX];
    return fs_read(m->fs, sub_path(m, real, sub, (int)sizeof sub), buf, max);
}

int vfs_write(const char *path, const void *buf, int size)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;

    struct vcred c; cred_now(&c);

    /* Writing to a synthetic node is a command, not a file update; the write
     * permission on the node is what gates it. */
    int n = kdiag_write(abs, buf, size);
    if (n != KDIAG_NOT_MINE) {
        if ((rc = check_file(abs, &c, MAY_WRITE)) < 0) return rc;
        return n;
    }
    if (vfsctl_size(abs) != KDIAG_NOT_MINE) {
        if ((rc = check_file(abs, &c, MAY_WRITE)) < 0) return rc;
        return vfsctl_write(abs, buf, size);
    }

    const char *real = vmeta_canon(abs);
    struct vmountent *m = mount_for(real);
    if (!m) return -1;
    char sub[VFS_PATH_MAX];
    const char *sp = sub_path(m, real, sub, (int)sizeof sub);

    /* An existing file needs write permission on itself; a new one needs write
     * permission on the directory that is about to gain a name. Checking only
     * the first is the classic hole: the file does not exist yet, so there is
     * nothing to refuse. */
    if (fs_size(m->fs, sp) >= 0) {
        if ((rc = check_file(real, &c, MAY_WRITE)) < 0) return rc;
    } else {
        if ((rc = check_parent_write(real, &c)) < 0) return rc;
    }
    return fs_write(m->fs, sp, buf, size);
}

int vfs_delete(const char *path)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 0);   /* unlink acts on the link */
    if (rc < 0) return rc;
    if (is_mount_point(abs)) return VFS_EBUSY;

    struct vcred c; cred_now(&c);
    if ((rc = check_parent_write(abs, &c)) < 0) return rc;

    /* A symlink is a VFS-level object with no backing file: dropping the
     * record IS the unlink. */
    struct vattr la;
    if (vmeta_lookup(abs, &la) && la.type == VT_LNK) {
        char promote[VFS_PATH_MAX];
        vmeta_unlink(abs, promote, (int)sizeof promote);
        return 0;
    }

    const char *canon = vmeta_canon(abs);
    int this_is_canon = s_eq(canon, abs);
    char promote[VFS_PATH_MAX];
    int must_move = vmeta_unlink(abs, promote, (int)sizeof promote);

    struct vmountent *m = mount_for(abs);
    if (!m) return -1;
    char sub[VFS_PATH_MAX], sub2[VFS_PATH_MAX];

    if (must_move) {
        /* This name held the bytes and other names for the same file survive.
         * A real filesystem would just decrement the inode's link count; with
         * a path-addressed backend the bytes have to move to a surviving name,
         * which is observably the same thing. */
        struct vmountent *m2 = mount_for(promote);
        if (m2 == m)
            return fs_rename(m->fs, sub_path(m, abs, sub, (int)sizeof sub),
                             sub_path(m2, promote, sub2, (int)sizeof sub2));
        return VFS_EXDEV;
    }
    if (!this_is_canon) return 0;    /* another name holds the bytes; nothing on disk to do */
    return fs_del(m->fs, sub_path(m, abs, sub, (int)sizeof sub));
}

int vfs_mkdir(const char *path)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 0);
    if (rc < 0) return rc;
    struct vcred c; cred_now(&c);
    if ((rc = check_parent_write(abs, &c)) < 0) return rc;

    struct vmountent *m = mount_for(abs);
    if (!m) return -1;
    char sub[VFS_PATH_MAX];
    rc = fs_mkdir(m->fs, sub_path(m, abs, sub, (int)sizeof sub));
    /* A directory created by a non-root process belongs to that process, or
     * the creator cannot put anything in it. */
    if (rc >= 0 && (c.uid || c.gid)) vmeta_chown(abs, 1, c.uid, c.gid);
    return rc;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char ao[VFS_PATH_MAX], an[VFS_PATH_MAX];
    int rc = resolve(old_path, ao, (int)sizeof ao, 0);
    if (rc < 0) return rc;
    rc = resolve(new_path, an, (int)sizeof an, 0);
    if (rc < 0) return rc;
    if (is_mount_point(ao) || is_mount_point(an)) return VFS_EBUSY;

    struct vcred c; cred_now(&c);
    if ((rc = check_parent_write(ao, &c)) < 0) return rc;
    if ((rc = check_parent_write(an, &c)) < 0) return rc;

    struct vmountent *mo = mount_for(ao), *mn = mount_for(an);
    if (!mo || !mn) return VFS_ENOENT;
    /* Rename is a directory-entry operation. Across a mount boundary there is
     * no single directory to operate in, so it is EXDEV -- the same answer
     * Linux gives, and the reason `mv` falls back to copy+delete. */
    if (mo != mn) return VFS_EXDEV;

    char so[VFS_PATH_MAX], sn[VFS_PATH_MAX];
    rc = fs_rename(mo->fs, sub_path(mo, ao, so, (int)sizeof so),
                   sub_path(mn, an, sn, (int)sizeof sn));
    if (rc >= 0) vmeta_renamed(ao, an);
    return rc;
}

/* --- directory enumeration ------------------------------------------------
 * Synthetic entries come first and the backend's follow, in one merged index
 * space. The old code returned the synthetic list INSTEAD of the backend's,
 * which meant a directory that both provided (there is one: /dev, once the VFS
 * grew nodes of its own) showed only half its contents. */

static int syn_count(const char *dir)
{
    int a = kdiag_dir_count(dir), b = vfsctl_dir_count(dir);
    int n = 0;
    if (a != KDIAG_NOT_MINE) n += a;
    if (b != KDIAG_NOT_MINE) n += b;
    return (a == KDIAG_NOT_MINE && b == KDIAG_NOT_MINE) ? KDIAG_NOT_MINE : n;
}

int vfs_count(const char *dir)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(dir, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;

    struct vcred c; cred_now(&c);
    int syn = syn_count(abs);

    struct vmountent *m = mount_for(abs);
    char sub[VFS_PATH_MAX];
    int back = m ? fs_count(m->fs, sub_path(m, abs, sub, (int)sizeof sub)) : -1;

    if (syn == KDIAG_NOT_MINE && back < 0) return back;
    /* Listing a directory needs read permission on it. Search (x) is what the
     * walk already checked; r is what turns "I may pass through" into "I may
     * see what is in here". */
    if ((rc = check_file(abs, &c, MAY_READ)) < 0) return rc;
    return (syn == KDIAG_NOT_MINE ? 0 : syn) + (back < 0 ? 0 : back);
}

/* Split a merged index into (provider, local index). */
static int syn_split(const char *abs, int i, int *which)
{
    int a = kdiag_dir_count(abs);
    if (a != KDIAG_NOT_MINE) { if (i < a) { *which = 0; return i; } i -= a; }
    int b = vfsctl_dir_count(abs);
    if (b != KDIAG_NOT_MINE) { if (i < b) { *which = 1; return i; } i -= b; }
    *which = 2;
    return i;
}

const char *vfs_ent_name(const char *dir, int i)
{
    char abs[VFS_PATH_MAX];
    if (resolve(dir, abs, (int)sizeof abs, 1) < 0) return "";
    int which, li = syn_split(abs, i, &which);
    if (which == 0) { const char *s = kdiag_dir_name(abs, li); return s ? s : ""; }
    if (which == 1) { const char *s = vfsctl_dir_name(abs, li); return s ? s : ""; }
    struct vmountent *m = mount_for(abs);
    if (!m) return "";
    char sub[VFS_PATH_MAX];
    return fs_ent_name(m->fs, sub_path(m, abs, sub, (int)sizeof sub), li);
}

int vfs_ent_size(const char *dir, int i)
{
    char abs[VFS_PATH_MAX];
    if (resolve(dir, abs, (int)sizeof abs, 1) < 0) return 0;
    int which, li = syn_split(abs, i, &which);
    if (which == 0) { int n = kdiag_dir_size(abs, li); return n == KDIAG_NOT_MINE ? 0 : n; }
    if (which == 1) { int n = vfsctl_dir_size(abs, li); return n == KDIAG_NOT_MINE ? 0 : n; }
    struct vmountent *m = mount_for(abs);
    if (!m) return 0;
    char sub[VFS_PATH_MAX];
    return fs_ent_size(m->fs, sub_path(m, abs, sub, (int)sizeof sub), li);
}

int vfs_ent_is_dir(const char *dir, int i)
{
    char abs[VFS_PATH_MAX];
    if (resolve(dir, abs, (int)sizeof abs, 1) < 0) return 0;
    int which, li = syn_split(abs, i, &which);
    if (which != 2) return 0;                    /* synthetic nodes are files */
    struct vmountent *m = mount_for(abs);
    if (!m) return 0;
    char sub[VFS_PATH_MAX];
    return fs_ent_is_dir(m->fs, sub_path(m, abs, sub, (int)sizeof sub), li);
}

/* --------------------------------------------------------------------------
 * Ownership, mode, and links
 * ------------------------------------------------------------------------ */

/* chmod/chown are the owner's to do (or root's). Anyone else changing them is
 * how a permission model is bypassed without ever failing a check. */
static int owner_ok(const char *abs, const struct vcred *c)
{
    struct vattr a;
    attrs_of(abs, &a);
    return (c->uid == 0 || c->uid == a.uid) ? 0 : VFS_EPERM;
}

static int is_dir_path(const char *abs)
{
    if (s_eq(abs, "/")) return 1;
    struct vmountent *m = mount_for(abs);
    char sub[VFS_PATH_MAX];
    return m && is_dir_on(m, sub_path(m, abs, sub, (int)sizeof sub));
}

int vfs_chmod(const char *path, unsigned mode)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;
    struct vcred c; cred_now(&c);
    if ((rc = owner_ok(abs, &c)) < 0) return rc;

    struct vmountent *m = mount_for(abs);
    if (m && m->fs->getattr && m->fs->setattr) {
        struct vattr a; attrs_of(abs, &a); a.mode = mode & 0777;
        char sub[VFS_PATH_MAX];
        return m->fs->setattr(sub_path(m, abs, sub, (int)sizeof sub), &a);
    }
    return vmeta_chmod(abs, is_dir_path(abs), mode);
}

int vfs_chown(const char *path, unsigned uid, unsigned gid)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 1);
    if (rc < 0) return rc;
    struct vcred c; cred_now(&c);
    /* Giving a file away is root-only, everywhere, for the good reason that
     * otherwise a user can plant a setuid-shaped file on somebody else. */
    if (c.uid != 0) return VFS_EPERM;

    struct vmountent *m = mount_for(abs);
    if (m && m->fs->getattr && m->fs->setattr) {
        struct vattr a; attrs_of(abs, &a); a.uid = uid; a.gid = gid;
        char sub[VFS_PATH_MAX];
        return m->fs->setattr(sub_path(m, abs, sub, (int)sizeof sub), &a);
    }
    return vmeta_chown(abs, is_dir_path(abs), uid, gid);
}

int vfs_symlink(const char *target, const char *linkpath)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(linkpath, abs, (int)sizeof abs, 0);
    if (rc < 0) return rc;
    struct vcred c; cred_now(&c);
    if ((rc = check_parent_write(abs, &c)) < 0) return rc;
    if (vfs_size(abs) >= 0 || vfs_count(abs) >= 0) return VFS_EEXIST;
    return vmeta_symlink(target, abs, &c);
}

int vfs_readlink(const char *path, char *buf, int max)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 0);
    if (rc < 0) return rc;
    rc = vmeta_readlink(abs, buf, max);
    if (rc < 0) return rc;
    if (rc == 0) return VFS_EINVAL;              /* not a symlink */
    return s_len(buf);
}

int vfs_link(const char *oldpath, const char *newpath)
{
    char ao[VFS_PATH_MAX], an[VFS_PATH_MAX];
    int rc = resolve(oldpath, ao, (int)sizeof ao, 0);
    if (rc < 0) return rc;
    rc = resolve(newpath, an, (int)sizeof an, 0);
    if (rc < 0) return rc;

    /* A hard link is a second directory entry for one inode. Across a mount
     * boundary there is no one inode, so it is EXDEV -- not a silent copy,
     * which is what a caller would get if this were allowed to succeed. */
    if (mount_for(ao) != mount_for(an)) return VFS_EXDEV;
    /* Directories first: a directory has no size, so an "does it exist" test
     * by size would report EPERM's case as ENOENT and say the wrong thing. */
    if (is_dir_path(ao)) return VFS_EPERM;
    if (vfs_size(ao) < 0) return VFS_ENOENT;
    if (vfs_size(an) >= 0 || vfs_count(an) >= 0) return VFS_EEXIST;

    struct vcred c; cred_now(&c);
    if ((rc = check_parent_write(an, &c)) < 0) return rc;
    if ((rc = check_file(ao, &c, MAY_READ)) < 0) return rc;

    return vmeta_link(ao, an, is_dir_path(ao));
}

int vfs_nlink(const char *path)
{
    char abs[VFS_PATH_MAX];
    int rc = resolve(path, abs, (int)sizeof abs, 0);
    if (rc < 0) return rc;
    return vmeta_nlink(abs);
}
