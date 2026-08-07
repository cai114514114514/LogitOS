/* The VFS metadata store. See vfs_meta.h for what it holds and why it is here
 * rather than in the inode. No kernel headers: this compiles into the host unit
 * tests alongside vfs.c. */

#include "vfs_meta.h"
#include "vfs_path.h"

struct vrec {
    char     path[VMETA_PATH];       /* "" = free slot */
    char     target[VMETA_TARGET];   /* VT_LNK only */
    uint32_t mode;
    uint32_t uid, gid;
    int      type;
    int      group;                  /* hard-link group id, 0 = none */
    int      canon;                  /* 1 = this name holds the bytes */
};

static struct vrec recs[VMETA_N];
static int next_group = 1;

static int m_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int m_eq(const char *a, const char *b)
{ int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }
static void m_cpy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

void vmeta_reset(void)
{
    for (int i = 0; i < VMETA_N; i++) recs[i].path[0] = 0;
    next_group = 1;
}

static struct vrec *find(const char *path)
{
    if (!path || !path[0]) return 0;
    for (int i = 0; i < VMETA_N; i++)
        if (recs[i].path[0] && m_eq(recs[i].path, path)) return &recs[i];
    return 0;
}

/* Create a record seeded from the current effective attributes, so that a
 * chmod of one bit does not silently reset the owner to root. */
static struct vrec *intern(const char *path, int is_dir)
{
    struct vrec *r = find(path);
    if (r) return r;
    if (!path || !path[0]) return 0;
    if (m_len(path) + 1 > VMETA_PATH) return 0;
    for (int i = 0; i < VMETA_N; i++) {
        if (recs[i].path[0]) continue;
        r = &recs[i];
        m_cpy(r->path, path, VMETA_PATH);
        r->target[0] = 0;
        r->type = is_dir ? VT_DIR : VT_REG;
        r->mode = is_dir ? 0755 : 0644;
        r->uid = 0; r->gid = 0;
        r->group = 0; r->canon = 1;
        return r;
    }
    return 0;                        /* store full: the caller reports ENOSPC */
}

int vmeta_lookup(const char *path, struct vattr *a)
{
    struct vrec *r = find(path);
    if (!r) return 0;
    if (a) {
        a->mode = r->mode; a->uid = r->uid; a->gid = r->gid; a->type = r->type;
        a->nlink = vmeta_nlink(path);
    }
    return 1;
}

void vmeta_attr(const char *path, int is_dir, struct vattr *a)
{
    if (!a) return;
    if (vmeta_lookup(path, a)) return;
    a->mode = is_dir ? 0755 : 0644;
    a->uid = 0; a->gid = 0;
    a->type = is_dir ? VT_DIR : VT_REG;
    a->nlink = 1;
}

int vmeta_chmod(const char *path, int is_dir, uint32_t mode)
{
    struct vrec *r = intern(path, is_dir);
    if (!r) return VFS_ENOSPC;
    r->mode = mode & 0777;
    /* A chmod applies to the file, and every name of a hard-linked file is the
     * same file. Propagate across the group so `chmod 600 a` is not undone by
     * reading through `b`. */
    if (r->group)
        for (int i = 0; i < VMETA_N; i++)
            if (recs[i].path[0] && recs[i].group == r->group) recs[i].mode = r->mode;
    return 0;
}

int vmeta_chown(const char *path, int is_dir, uint32_t uid, uint32_t gid)
{
    struct vrec *r = intern(path, is_dir);
    if (!r) return VFS_ENOSPC;
    r->uid = uid; r->gid = gid;
    if (r->group)
        for (int i = 0; i < VMETA_N; i++)
            if (recs[i].path[0] && recs[i].group == r->group) {
                recs[i].uid = uid; recs[i].gid = gid;
            }
    return 0;
}

int vmeta_symlink(const char *target, const char *path, const struct vcred *c)
{
    if (!target || !target[0] || !path || !path[0]) return VFS_EINVAL;
    if (m_len(target) + 1 > VMETA_TARGET) return VFS_ENAMETOOLONG;
    if (find(path)) return VFS_EEXIST;
    struct vrec *r = intern(path, 0);
    if (!r) return VFS_ENOSPC;
    r->type = VT_LNK;
    r->mode = 0777;                  /* a symlink's own mode is not consulted */
    if (c) { r->uid = c->uid; r->gid = c->gid; }
    m_cpy(r->target, target, VMETA_TARGET);
    return 0;
}

int vmeta_readlink(const char *path, char *out, int max)
{
    struct vrec *r = find(path);
    if (!r || r->type != VT_LNK) return 0;
    int n = m_len(r->target);
    if (n + 1 > max) return VFS_ENAMETOOLONG;
    m_cpy(out, r->target, max);
    return 1;
}

/* --- hard links --------------------------------------------------------- */

const char *vmeta_canon(const char *path)
{
    struct vrec *r = find(path);
    if (!r || !r->group || r->canon) return path;
    for (int i = 0; i < VMETA_N; i++)
        if (recs[i].path[0] && recs[i].group == r->group && recs[i].canon)
            return recs[i].path;
    return path;
}

int vmeta_nlink(const char *path)
{
    struct vrec *r = find(path);
    if (!r || !r->group) return 1;
    int n = 0;
    for (int i = 0; i < VMETA_N; i++)
        if (recs[i].path[0] && recs[i].group == r->group) n++;
    return n ? n : 1;
}

int vmeta_link(const char *oldpath, const char *newpath, int is_dir)
{
    if (is_dir) return VFS_EPERM;                /* no hard links to directories */
    if (find(newpath)) return VFS_EEXIST;
    struct vrec *o = intern(oldpath, 0);
    if (!o) return VFS_ENOSPC;
    if (o->type == VT_LNK) return VFS_EPERM;     /* link() does not follow, and a
                                                  * link to a symlink record would
                                                  * make two names one link */
    struct vrec *n = intern(newpath, 0);
    if (!n) return VFS_ENOSPC;
    if (!o->group) { o->group = next_group++; o->canon = 1; }
    n->group = o->group;
    n->canon = 0;
    n->type = o->type; n->mode = o->mode; n->uid = o->uid; n->gid = o->gid;
    return vmeta_nlink(oldpath);
}

int vmeta_unlink(const char *path, char *promote, int max)
{
    struct vrec *r = find(path);
    if (promote && max > 0) promote[0] = 0;
    if (!r) return 0;
    if (!r->group) { r->path[0] = 0; return 0; }

    int grp = r->group, was_canon = r->canon;
    r->path[0] = 0;                              /* drop this name */

    struct vrec *survivor = 0;
    int count = 0;
    for (int i = 0; i < VMETA_N; i++)
        if (recs[i].path[0] && recs[i].group == grp) { count++; if (!survivor) survivor = &recs[i]; }

    if (count == 0) return 0;                    /* last name: the bytes go too */
    if (count == 1) survivor->group = 0;         /* back to an ordinary file */
    if (!was_canon) return 0;                    /* the bytes live elsewhere; nothing to do */

    survivor->canon = 1;
    if (promote) m_cpy(promote, survivor->path, max);
    return 1;                                    /* caller must move the data */
}

void vmeta_renamed(const char *from, const char *to)
{
    struct vrec *r = find(from);
    if (!r) return;
    if (m_len(to) + 1 > VMETA_PATH) { r->path[0] = 0; return; }
    m_cpy(r->path, to, VMETA_PATH);
}

void vmeta_forget_subtree(const char *prefix)
{
    for (int i = 0; i < VMETA_N; i++)
        if (recs[i].path[0] && vfs_path_is_prefix(prefix, recs[i].path))
            recs[i].path[0] = 0;
}

/* --- the check ---------------------------------------------------------- */

int vmeta_permission(const struct vattr *a, const struct vcred *c, int want)
{
    if (!a || !c) return VFS_EACCES;

#ifdef VFS_NEGCTL_STORE_ONLY
    /* THE NEGATIVE CONTROL (make test-vfs-negctl, and see the recipe in
     * tests/boot/run-vfs-test.sh for the on-device form).
     *
     * Under this define the mode, the owner and the group are all stored, all
     * readable back through stat, and never once consulted. That is exactly
     * what a permission model that is not enforced looks like from outside,
     * and it is what this layer has to be distinguishable from -- so every
     * refusal assertion in both suites must fail here. If they do not, they
     * were not testing enforcement. */
    (void)want;
    return 0;
#endif

    if (!(want & (MAY_READ | MAY_WRITE | MAY_EXEC))) return 0;

    if (c->uid == 0) {
        /* root reads and writes anything. It does NOT execute a file with no
         * execute bit set anywhere -- that is Linux's rule, and it is the one
         * that stops `./notes.txt` from running as root. */
        if ((want & MAY_EXEC) && !(a->mode & (VM_IXUSR | VM_IXGRP | VM_IXOTH)))
            return VFS_EACCES;
        return 0;
    }

    uint32_t bits;
    if (c->uid == a->uid)      bits = (a->mode >> 6) & 7;
    else if (c->gid == a->gid) bits = (a->mode >> 3) & 7;
    else                       bits =  a->mode       & 7;

    /* MAY_* are numerically r=4 w=2 x=1, which is the same numbering as the
     * mode bits, so the test is a mask and not a case analysis. */
    return ((bits & (uint32_t)want) == (uint32_t)want) ? 0 : VFS_EACCES;
}

static int put(char *b, int max, int n, const char *s)
{ for (int i = 0; s[i] && n < max; i++) b[n++] = s[i]; return n; }

static int putn(char *b, int max, int n, unsigned long v, int oct)
{
    char t[24]; int k = 0;
    unsigned long base = oct ? 8 : 10;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + (v % base)); v /= base; }
    while (k && n < max) b[n++] = t[--k];
    return n;
}

int vmeta_render(char *buf, int max)
{
    int n = 0;
    n = put(buf, max, n, "# path mode uid gid type nlink target\n");
    for (int i = 0; i < VMETA_N; i++) {
        if (!recs[i].path[0]) continue;
        n = put(buf, max, n, recs[i].path);
        n = put(buf, max, n, " 0");
        n = putn(buf, max, n, recs[i].mode, 1);
        n = put(buf, max, n, " ");
        n = putn(buf, max, n, recs[i].uid, 0);
        n = put(buf, max, n, " ");
        n = putn(buf, max, n, recs[i].gid, 0);
        n = put(buf, max, n, recs[i].type == VT_DIR ? " dir " :
                             recs[i].type == VT_LNK ? " lnk " : " reg ");
        n = putn(buf, max, n, (unsigned long)vmeta_nlink(recs[i].path), 0);
        if (recs[i].target[0]) { n = put(buf, max, n, " -> "); n = put(buf, max, n, recs[i].target); }
        n = put(buf, max, n, "\n");
    }
    if (n < max) buf[n] = 0;
    return n;
}
