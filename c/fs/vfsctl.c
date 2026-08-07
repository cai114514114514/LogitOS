/* The VFS control and introspection nodes. See vfsctl.h for the command set
 * and for why this is a file rather than six syscalls. */

#include <stddef.h>
#include <stdint.h>
#include "vfsctl.h"
#include "fsbench.h"
#include "vfs.h"
#include "vfs_meta.h"
#include "vfs_cred.h"
#include "ramfs.h"
#include "lfsro.h"
#include "proc.h"
#include "file.h"
#include "logit_abi.h"    /* O_* */

/* The same "not mine" sentinel c/fs/vfs.c uses. Defined here rather than
 * included from kdiag.h: this facility must not fail to compile because
 * another line has its diagnostic files half-landed. */
#define KDIAG_NOT_MINE (-2)

#define CTL_RESULT_MAX 1024

static char g_result[CTL_RESULT_MAX] = "";
static int  g_result_len;

static int  c_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int  c_eq(const char *a, const char *b)
{ int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }

static const char *g_names[] = { "vfsctl", "vfsmounts", "vfsmeta", "fsbench" };
#define NODE_FSBENCH 3
#define NCTL ((int)(sizeof g_names / sizeof g_names[0]))

static int which_node(const char *path)
{
    if (!path) return -1;
    for (int i = 0; i < NCTL; i++) {
        /* "/dev/" + name */
        int k = 0;
        const char *p = path, *n = g_names[i];
        if (p[0] != '/' || p[1] != 'd' || p[2] != 'e' || p[3] != 'v' || p[4] != '/') return -1;
        p += 5;
        for (; n[k]; k++) if (p[k] != n[k]) break;
        if (!n[k] && !p[k]) return i;
    }
    return -1;
}

/* --- result formatting --------------------------------------------------- */

static void r_reset(void) { g_result_len = 0; g_result[0] = 0; }
static void r_put(const char *s)
{
    for (int i = 0; s && s[i] && g_result_len < CTL_RESULT_MAX - 1; i++)
        g_result[g_result_len++] = s[i];
    g_result[g_result_len] = 0;
}
static void r_num(long v, int oct)
{
    char t[24]; int k = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    unsigned long base = oct ? 8 : 10;
    if (v < 0) r_put("-");
    if (!u) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + u % base); u /= base; }
    while (k && g_result_len < CTL_RESULT_MAX - 1) g_result[g_result_len++] = t[--k];
    g_result[g_result_len] = 0;
}

/* Every command answers with "ok ..." or "err <name> <n>". A test greps for
 * the word, so the word has to be unambiguous: "err" never appears in a
 * success line. */
static int fail(const char *what, int rc)
{
    r_reset(); r_put("err "); r_put(what); r_put(" "); r_num(rc, 0); r_put("\n");
    return rc;
}
static int okay(void) { r_put("\n"); return 0; }

/* --- argument parsing ---------------------------------------------------- */

struct args { char a[5][128]; int n; };

static void split(const char *buf, int len, struct args *A)
{
    A->n = 0;
    int i = 0;
    while (i < len && A->n < 5) {
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= len || !buf[i]) break;
        int k = 0;
        while (i < len && buf[i] && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r') {
            if (k < 127) A->a[A->n][k++] = buf[i];
            i++;
        }
        A->a[A->n][k] = 0;
        A->n++;
    }
}

static long num(const char *s, int base)
{
    long v = 0; int i = 0, neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    for (; s[i]; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    return neg ? -v : v;
}

/* --- the commands -------------------------------------------------------- */

static int cmd_stat(const char *path)
{
    struct vattr a;
    int rc = vfs_stat(path, &a);
    if (rc < 0) return fail("stat", rc);
    r_reset();
    r_put("ok mode 0"); r_num((long)a.mode, 1);
    r_put(" uid "); r_num((long)a.uid, 0);
    r_put(" gid "); r_num((long)a.gid, 0);
    r_put(" nlink "); r_num((long)a.nlink, 0);
    r_put(a.type == VT_DIR ? " dir" : a.type == VT_LNK ? " lnk" : " reg");
    r_put(" size "); r_num((long)vfs_size(path), 0);
    return okay();
}

static int cmd_mount(struct args *A)
{
    if (A->n < 4) return fail("usage", VFS_EINVAL);
    struct filesystem *fs = NULL;
    if (c_eq(A->a[1], "ramfs"))      fs = ramfs_create(A->a[2]);
    else if (c_eq(A->a[1], "lfsro")) fs = lfsro_create(A->a[2]);
    else return fail("fstype", VFS_EINVAL);
    if (!fs) return fail("create", VFS_ENOENT);

    int rc = vfs_mount_at(A->a[3], fs);
    if (rc < 0) {
        if (c_eq(A->a[1], "ramfs")) ramfs_destroy(fs); else lfsro_destroy(fs);
        return fail("mount", rc);
    }
    r_reset(); r_put("ok mounted "); r_put(A->a[1]); r_put(" ");
    r_put(A->a[2]); r_put(" at "); r_put(A->a[3]);
    return okay();
}

static int cmd_su(struct args *A, int to_parent)
{
    struct proc *p = proc_current();
    if (!p) return fail("noproc", VFS_EPERM);
    int target = to_parent ? p->ppid : p->pid;
    if (target <= 0) return fail("noparent", VFS_EPERM);
    uint32_t uid = (uint32_t)num(A->a[1], 10);
    uint32_t gid = A->n > 2 ? (uint32_t)num(A->a[2], 10) : uid;
    int rc = vfs_cred_set(target, uid, gid);
    if (rc < 0) return fail("setcred", rc);
    r_reset(); r_put("ok pid "); r_num(target, 0);
    r_put(" uid "); r_num((long)uid, 0); r_put(" gid "); r_num((long)gid, 0);
    return okay();
}

/* Exercise the open-file-description layer in the CALLING process's fd table,
 * on the real machine.
 *
 * The claim being tested is the one that separates a descriptor from a
 * description: dup(2) makes a second DESCRIPTOR onto ONE description, so the
 * file offset is shared. Read three bytes through the first and three through
 * the second: if the description is shared the second read returns DEF, and if
 * each descriptor secretly had its own offset it returns ABC. There is no way
 * to tell those two implementations apart by inspection, and every other test
 * in this tree passes under both.
 *
 * fork(2) shares descriptions through exactly this path -- proc_fork() calls
 * file_dup() on each inherited fd, which is the same refcount share dup uses --
 * so the refcount half is checked here too: closing one descriptor must leave
 * the other usable, which is what makes an inherited fd survive the parent's
 * close.
 *
 * Runs in the context of whichever ring-3 process wrote the command, using its
 * own fd table, and leaves nothing behind. */
static int cmd_fdtest(const char *path)
{
    struct proc *p = proc_current();
    if (!p) return fail("noproc", VFS_EPERM);

    struct file *w = file_open_vfs(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!w) return fail("create", VFS_EACCES);
    if (file_write(w, "ABCDEF", 6) != 6) { file_close(w); return fail("seed", VFS_EIO); }
    if (file_read(w, (char[4]){0}, 3) != -1) { file_close(w); return fail("wronly-readable", VFS_EIO); }
    file_close(w);                                   /* the last close flushes */

    struct file *f = file_open_vfs(path, O_RDONLY);
    if (!f) return fail("open", VFS_EACCES);
    if (file_write(f, "x", 1) != -1) { file_close(f); return fail("rdonly-writable", VFS_EIO); }

    int fda = proc_fd_alloc(p, f);
    if (fda < 0) { file_close(f); return fail("fdtable", VFS_EMFILE); }
    file_dup(f);                                     /* dup(2): +1 descriptor, same description */
    int fdb = proc_fd_alloc(p, f);
    if (fdb < 0) {                       /* two references are live: the open and the dup */
        p->fd[fda] = 0; file_close(f); file_close(f);
        return fail("fdtable", VFS_EMFILE);
    }

    char a[4] = { 0 }, b[4] = { 0 };
    file_read(proc_fd_get(p, fda), a, 3);
    file_read(proc_fd_get(p, fdb), b, 3);

    /* Close one descriptor; the description must survive for the other. */
    file_close(p->fd[fda]); p->fd[fda] = 0;
    char c[4] = { 0 };
    long after = file_read(proc_fd_get(p, fdb), c, 1);
    file_close(p->fd[fdb]); p->fd[fdb] = 0;

    int shared   = (a[0] == 'A' && a[1] == 'B' && a[2] == 'C' &&
                    b[0] == 'D' && b[1] == 'E' && b[2] == 'F');
    int survived = (after == 0);                     /* at EOF after 6 of 6 bytes */

    r_reset();
    r_put(shared && survived ? "ok fdtest" : "err fdtest");
    r_put(" first="); r_put(a);
    r_put(" second="); r_put(b);
    r_put(shared ? " OFFSET-SHARED" : " OFFSET-NOT-SHARED");
    r_put(survived ? " DESCRIPTION-SURVIVED-CLOSE" : " DESCRIPTION-LOST");
    r_put("\n");
    return shared && survived ? 0 : VFS_EIO;
}

static int run(const char *buf, int len)
{
    struct args A;
    split(buf, len, &A);
    if (A.n == 0) return fail("empty", VFS_EINVAL);
    const char *c = A.a[0];

    if (c_eq(c, "chmod")) {
        if (A.n < 3) return fail("usage", VFS_EINVAL);
        int rc = vfs_chmod(A.a[2], (unsigned)num(A.a[1], 8));
        if (rc < 0) return fail("chmod", rc);
        r_reset(); r_put("ok chmod "); r_put(A.a[2]); return okay();
    }
    if (c_eq(c, "chown")) {
        if (A.n < 4) return fail("usage", VFS_EINVAL);
        int rc = vfs_chown(A.a[3], (unsigned)num(A.a[1], 10), (unsigned)num(A.a[2], 10));
        if (rc < 0) return fail("chown", rc);
        r_reset(); r_put("ok chown "); r_put(A.a[3]); return okay();
    }
    if (c_eq(c, "ln")) {
        int sym = A.n > 1 && c_eq(A.a[1], "-s");
        const char *a1 = sym ? A.a[2] : A.a[1];
        const char *a2 = sym ? A.a[3] : A.a[2];
        if (A.n < (sym ? 4 : 3)) return fail("usage", VFS_EINVAL);
        int rc = sym ? vfs_symlink(a1, a2) : vfs_link(a1, a2);
        if (rc < 0) return fail(sym ? "symlink" : "link", rc);
        r_reset(); r_put("ok "); r_put(sym ? "symlink " : "link ");
        r_put(a2); r_put(" nlink "); r_num(vfs_nlink(a2), 0); return okay();
    }
    if (c_eq(c, "stat")) {
        if (A.n < 2) return fail("usage", VFS_EINVAL);
        return cmd_stat(A.a[1]);
    }
    if (c_eq(c, "readlink")) {
        if (A.n < 2) return fail("usage", VFS_EINVAL);
        char t[VFS_PATH_MAX];
        int rc = vfs_readlink(A.a[1], t, (int)sizeof t);
        if (rc < 0) return fail("readlink", rc);
        r_reset(); r_put("ok -> "); r_put(t); return okay();
    }
    if (c_eq(c, "access")) {
        if (A.n < 3) return fail("usage", VFS_EINVAL);
        int want = 0;
        for (int i = 0; A.a[2][i]; i++) {
            if (A.a[2][i] == 'r') want |= MAY_READ;
            if (A.a[2][i] == 'w') want |= MAY_WRITE;
            if (A.a[2][i] == 'x') want |= MAY_EXEC;
        }
        int rc = vfs_access(A.a[1], want);
        if (rc < 0) return fail("access", rc);
        r_reset(); r_put("ok access "); r_put(A.a[1]); r_put(" "); r_put(A.a[2]);
        return okay();
    }
    if (c_eq(c, "fdtest")) {
        if (A.n < 2) return fail("usage", VFS_EINVAL);
        return cmd_fdtest(A.a[1]);
    }
    if (c_eq(c, "mount"))  return cmd_mount(&A);
    if (c_eq(c, "umount")) {
        if (A.n < 2) return fail("usage", VFS_EINVAL);
        int rc = vfs_umount(A.a[1]);
        if (rc < 0) return fail("umount", rc);
        r_reset(); r_put("ok umount "); r_put(A.a[1]); return okay();
    }
    if (c_eq(c, "id")) {
        struct vcred cr; vfs_cred_current(&cr);
        struct proc *p = proc_current();
        r_reset(); r_put("ok pid "); r_num(p ? p->pid : -1, 0);
        r_put(" ppid "); r_num(p ? p->ppid : -1, 0);
        r_put(" uid "); r_num((long)cr.uid, 0);
        r_put(" gid "); r_num((long)cr.gid, 0);
        return okay();
    }
    if (c_eq(c, "setuid")) { if (A.n < 2) return fail("usage", VFS_EINVAL); return cmd_su(&A, 0); }
    if (c_eq(c, "su"))     { if (A.n < 2) return fail("usage", VFS_EINVAL); return cmd_su(&A, 1); }

    /* Quote the word that was not understood. "err command -22" tells you
     * nothing about whether the command was wrong or the bytes were. */
    r_reset(); r_put("err command -22 got '"); r_put(c); r_put("' argc ");
    r_num(A.n, 0); r_put("\n");
    return VFS_EINVAL;
}

/* --- the node interface -------------------------------------------------- */

int vfsctl_size(const char *path)
{
    int w = which_node(path);
    if (w < 0) return KDIAG_NOT_MINE;
    if (w == 0) return g_result_len;
    if (w == NODE_FSBENCH) return fsbench_len();
    char tmp[4096];
    return w == 1 ? vfs_mounts_render(tmp, (int)sizeof tmp)
                  : vmeta_render(tmp, (int)sizeof tmp) + vfs_cred_render(tmp, (int)sizeof tmp);
}

int vfsctl_read(const char *path, void *buf, int max)
{
    int w = which_node(path);
    if (w < 0) return KDIAG_NOT_MINE;
    char *out = (char *)buf;
    if (w == 0) {
        int n = g_result_len < max ? g_result_len : max;
        for (int i = 0; i < n; i++) out[i] = g_result[i];
        return n;
    }
    if (w == NODE_FSBENCH) return fsbench_render(out, max);
    if (w == 1) return vfs_mounts_render(out, max);
    int n = vmeta_render(out, max);
    if (n < max) n += vfs_cred_render(out + n, max - n);
    return n;
}

int vfsctl_write(const char *path, const void *buf, int len)
{
    int w = which_node(path);
    if (w < 0) return KDIAG_NOT_MINE;
    if (w == NODE_FSBENCH) { if (len > 0) fsbench_command((const char *)buf, len); return len; }
    if (w != 0) return -1;                    /* the rendered views are read-only */
    if (len <= 0) return 0;
    run((const char *)buf, len);
    /* Report the write as accepted whatever the command answered: the caller
     * asked to deliver a command and it was delivered. The command's own
     * verdict is in the result, which is what a test reads -- collapsing the
     * two would make `echo` fail for a refused chmod and hide which of the two
     * refused. */
    return len;
}

int vfsctl_dir_count(const char *dir)
{
    if (dir && (c_eq(dir, "/dev") || c_eq(dir, "/dev/"))) return NCTL;
    return KDIAG_NOT_MINE;
}

const char *vfsctl_dir_name(const char *dir, int i)
{
    if (dir && (c_eq(dir, "/dev") || c_eq(dir, "/dev/")) && i >= 0 && i < NCTL)
        return g_names[i];
    return 0;
}

int vfsctl_dir_size(const char *dir, int i)
{
    if (!dir || !(c_eq(dir, "/dev") || c_eq(dir, "/dev/")) || i < 0 || i >= NCTL)
        return KDIAG_NOT_MINE;
    char p[32];
    int k = 0;
    const char *pre = "/dev/";
    for (; pre[k]; k++) p[k] = pre[k];
    for (int j = 0; g_names[i][j] && k < 31; j++) p[k++] = g_names[i][j];
    p[k] = 0;
    int n = vfsctl_size(p);
    (void)c_len("");
    return n < 0 ? 0 : n;
}
