/* Process credentials for the VFS. See vfs_cred.h for why they live here
 * rather than in struct proc, and what proc.c should grow to take them back.
 *
 * M32: this file used to hold a table nothing could write. vfs_cred_set()
 * existed, the VFS consulted it on every path walk, and the only caller was a
 * /dev/vfsctl debug command -- so in practice every process on the machine was
 * root and every permission check compared a file's owner against 0 and
 * passed. The enforcement was real; the identity was a constant. What is added
 * here is the identity: supplementary groups, the login SESSION, and the
 * kernel side of the SYS_GETUID..SYS_GETSESSION block (150-159). */

#include <stdint.h>
#include <stddef.h>
#include "vfs_cred.h"
#include "vfs_path.h"
#include "proc.h"
#include "spinlock.h"
#include "logit_abi.h"
#include "usercopy.h"

#define NCRED 32

struct centry {
    int      pid;          /* 0 = free */
    uint32_t uid, gid;
    int      ngroups;
    uint32_t groups[ID_NGROUPS_MAX];
};

static struct centry creds[NCRED];
static spinlock_t g_cred_lock = SPINLOCK_INIT;

/* THE SESSION. See the block comment above SYS_SETSESSION in
 * include/abi/logit_abi.h for the full argument; the short form is that this
 * machine brings a desktop up before anything has authenticated, and those
 * processes descend from a kernel thread and so have no credential to inherit.
 * This is the credential of a process that was never told who it belongs to.
 * It starts as root because that is the boot state -- mounting the filesystem
 * and loading 2.2 MB of fonts happen before there is a userland to have a uid
 * -- and /bin/login replaces it with the person at the keyboard.
 *
 * It is deliberately NOT reset on logout, because there is no logout: this
 * machine has one seat and a session ends when it powers off. */
static uint32_t g_sess_uid, g_sess_gid;

/* Has anything on this machine ever been given a supplementary group? Read
 * without the lock on the hot permission path -- see vfs_cred_ingroup. It is
 * only ever set, never cleared, so a stale read can cost one unnecessary walk
 * and can never miss a membership. */
static volatile int g_any_groups;

/* Drop entries whose process is gone. Called when the table is full and when a
 * credential is set, which is rare enough that a linear sweep costs nothing.
 * Without it a long-lived system eventually hands a recycled pid somebody
 * else's uid -- and it would hand it a LOWER one, which is the direction that
 * matters. Caller holds g_cred_lock. */
static void sweep_locked(void)
{
    for (int i = 0; i < NCRED; i++)
        if (creds[i].pid && !proc_by_pid(creds[i].pid)) creds[i].pid = 0;
}

static struct centry *find_locked(int pid)
{
    for (int i = 0; i < NCRED; i++) if (creds[i].pid == pid) return &creds[i];
    return NULL;
}

/* The entry that GOVERNS `pid`: its own if it has one, otherwise the first one
 * found walking up the ppid chain, otherwise NULL (meaning: the session).
 *
 * Caller holds the lock and passes its saved flags, because this drops and
 * retakes the lock around proc_by_pid(), which takes its own. Bounded by NPROC
 * so a corrupted ppid cycle cannot spin here. */
static struct centry *govern_locked(int pid, uint64_t *fl)
{
    int cur = pid;
    for (int hop = 0; hop < NPROC && cur > 0; hop++) {
        struct centry *e = find_locked(cur);
        if (e) return e;
        spin_unlock_irqrestore(&g_cred_lock, *fl);
        struct proc *p = proc_by_pid(cur);           /* takes its own lock */
        *fl = spin_lock_irqsave(&g_cred_lock);
        if (!p) break;
        cur = p->ppid;
    }
    return NULL;
}

int vfs_cred_get(int pid, struct vcred *c)
{
    if (!c) return VFS_EINVAL;
    c->uid = 0; c->gid = 0;
    if (pid <= 0) return 0;

    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    struct centry *e = govern_locked(pid, &fl);
    if (e) { c->uid = e->uid; c->gid = e->gid; }
    else   { c->uid = g_sess_uid; c->gid = g_sess_gid; }
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return 0;
}

/* The pid the VFS keys per-process state on. Exists so that vfs.c -- which has
 * no kernel headers on purpose -- can reach the process table through one
 * declaration instead of including proc.h. */
int vfs_cred_pid(void)
{
    struct proc *p = proc_current();
    return p ? p->pid : 0;
}

void vfs_cred_current(struct vcred *c)
{
    if (!c) return;
    c->uid = 0; c->gid = 0;
    struct proc *p = proc_current();
    if (!p) return;                 /* kernel thread / early boot: root */
    vfs_cred_get(p->pid, c);
}

int vfs_cred_set(int pid, uint32_t uid, uint32_t gid)
{
    if (pid <= 0) return VFS_EINVAL;
    if (!proc_by_pid(pid)) return VFS_ENOENT;

    struct vcred me;
    vfs_cred_current(&me);
    /* The only rule, and the whole rule: root may become anybody; a process
     * that is not root may not change its uid, which is what makes dropping
     * privilege one-way. There is no setuid bit on any file in this system, so
     * there is no legitimate path back up. */
    if (me.uid != 0) return VFS_EPERM;

    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    sweep_locked();
    struct centry *e = find_locked(pid);
    if (!e) {
        for (int i = 0; i < NCRED; i++) if (!creds[i].pid) { e = &creds[i]; break; }
        if (!e) { spin_unlock_irqrestore(&g_cred_lock, fl); return VFS_ENOSPC; }
        e->pid = pid; e->ngroups = 0;
    }
    e->uid = uid; e->gid = gid;
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return 0;
}

/* --- supplementary groups ------------------------------------------------ */

/* Is the CURRENT process a member of `gid` other than through its primary
 * group? Called from vmeta_permission through a weak declaration, so the host
 * VFS unit test -- which does not link this file -- resolves it to NULL and
 * skips the check, exactly as it did before groups existed.
 *
 * The impurity worth naming: vmeta_permission is handed a `struct vcred`, and
 * this reads the CURRENT process instead of that struct. They are the same
 * thing at every call site (vfs.c's cred_now() fills the struct from
 * vfs_cred_current()), and the alternative was widening struct vcred, which is
 * a field in a header two other lines are building against. When the
 * credential moves into struct proc as vfs_cred.h describes, the group list
 * moves with it and this hook goes away. */
int vfs_cred_ingroup(uint32_t gid)
{
    /* THE FAST PATH IS THE POINT. This is called from vmeta_permission, which
     * runs once per path component of every path-taking syscall, and only on
     * the branch where the caller is neither the owner nor in the primary
     * group -- which for a logged-in user is every traversal of a root-owned
     * directory, i.e. `/` and `/bin` on every command. The slow path takes a
     * lock and walks the ppid chain. `g_any_groups` is set exactly once, by
     * the only function that can put a group in the table, so a machine on
     * which nothing has ever called setgroups -- every machine today -- pays
     * one relaxed load. */
    if (!g_any_groups) return 0;
    struct proc *p = proc_current();
    if (!p) return 0;
    int found = 0;
    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    struct centry *e = govern_locked(p->pid, &fl);
    if (e) for (int i = 0; i < e->ngroups; i++) if (e->groups[i] == gid) { found = 1; break; }
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return found;
}

int vfs_cred_groups_get(int pid, uint32_t *out, int max)
{
    if (pid <= 0) return 0;
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    struct centry *e = govern_locked(pid, &fl);
    if (e) {
        n = e->ngroups;
        if (out) for (int i = 0; i < n && i < max; i++) out[i] = e->groups[i];
    }
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return n;
}

int vfs_cred_groups_set(int pid, const uint32_t *list, int n)
{
    if (pid <= 0 || n < 0 || n > ID_NGROUPS_MAX) return VFS_EINVAL;
    if (!proc_by_pid(pid)) return VFS_ENOENT;

    struct vcred me;
    vfs_cred_current(&me);
    if (me.uid != 0) return VFS_EPERM;

    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    sweep_locked();
    /* A process given only groups keeps whatever uid it had, which is whatever
     * it inherits -- so read the governing credential BEFORE claiming a slot
     * (govern_locked drops the lock, and a slot with pid already written would
     * find itself), or `setgroups` would silently promote the caller to root. */
    struct centry *g = govern_locked(pid, &fl);
    uint32_t inh_uid = g ? g->uid : g_sess_uid, inh_gid = g ? g->gid : g_sess_gid;
    struct centry *e = find_locked(pid);
    if (!e) {
        for (int i = 0; i < NCRED; i++) if (!creds[i].pid) { e = &creds[i]; break; }
        if (!e) { spin_unlock_irqrestore(&g_cred_lock, fl); return VFS_ENOSPC; }
        e->pid = pid;
        e->uid = inh_uid;
        e->gid = inh_gid;
    }
    e->ngroups = n;
    for (int i = 0; i < n; i++) e->groups[i] = list[i];
    if (n) g_any_groups = 1;
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return 0;
}

/* --- the session --------------------------------------------------------- */

void vfs_cred_session(uint32_t *uid, uint32_t *gid)
{
    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    if (uid) *uid = g_sess_uid;
    if (gid) *gid = g_sess_gid;
    spin_unlock_irqrestore(&g_cred_lock, fl);
}

/* Establish the login session AND the caller's own credential, in one call.
 *
 * WHY IT IS ONE CALL. Done as two, /bin/login has to pick an order and both
 * are wrong. Set the session first and the caller -- which has no entry of its
 * own -- IS the new user from that instant, so the following setuid() is made
 * by a non-root process and is refused. Set the caller's uid first and it has
 * dropped root before it can set the session, so the desktop never learns who
 * logged in. Neither is a race in the concurrency sense; both are simply
 * unimplementable. So: one root check, one lock, both writes. */
int vfs_cred_session_set(uint32_t uid, uint32_t gid)
{
    struct proc *p = proc_current();
    struct vcred me;
    vfs_cred_current(&me);
    if (me.uid != 0) return VFS_EPERM;

    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    sweep_locked();
    g_sess_uid = uid; g_sess_gid = gid;
    if (p) {
        struct centry *e = find_locked(p->pid);
        if (!e) for (int i = 0; i < NCRED; i++) if (!creds[i].pid) { e = &creds[i]; e->pid = p->pid; e->ngroups = 0; break; }
        if (e) { e->uid = uid; e->gid = gid; }
    }
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return 0;
}

/* --- the syscalls (150-159) ---------------------------------------------- */

/* Forwarded whole from c/kernel/exec/syscall.c, for the reason meta_syscall()
 * gives: which argument is a user pointer and what it means are facts about
 * this subsystem. Kept in this file rather than a new one because every line
 * of it reads or writes the table above. */
long id_syscall(long num, long a, long b, long c)
{
    (void)c;
    struct proc *p = proc_current();
    struct vcred me;
    vfs_cred_current(&me);

    switch (num) {
    /* geteuid/getegid are deliberate aliases. See logit_abi.h: there is no
     * setuid bit in this filesystem, so there is nothing that could ever make
     * them differ, and a field that is always equal to another field is a
     * decoration a caller would be wrong to trust. */
    case SYS_GETUID:
    case SYS_GETEUID:  return (long)me.uid;
    case SYS_GETGID:
    case SYS_GETEGID:  return (long)me.gid;

    case SYS_SETUID:
        if (!p) return ID_E_NOPROC;
        if (me.uid != 0) return ID_E_PERM;
        return vfs_cred_set(p->pid, (uint32_t)a, me.gid) < 0 ? ID_E_PERM : 0;

    case SYS_SETGID:
        if (!p) return ID_E_NOPROC;
        if (me.uid != 0) return ID_E_PERM;
        return vfs_cred_set(p->pid, me.uid, (uint32_t)a) < 0 ? ID_E_PERM : 0;

    case SYS_GETGROUPS: {
        if (!p) return ID_E_NOPROC;
        int max = (int)a;
        if (max < 0 || max > ID_NGROUPS_MAX) return ID_E_ARG;
        uint32_t tmp[ID_NGROUPS_MAX];
        int n = vfs_cred_groups_get(p->pid, tmp, ID_NGROUPS_MAX);
        if (max == 0) return n;                 /* "how many?" -- b may be NULL */
        if (n > max) return ID_E_ARG;           /* POSIX: EINVAL, not truncation */
        if (n && (!b || user_copy_to((void *)b, tmp, (uint64_t)n * 4) < 0))
            return ID_E_ARG;
        return n;
    }

    case SYS_SETGROUPS: {
        if (!p) return ID_E_NOPROC;
        int n = (int)a;
        if (n < 0 || n > ID_NGROUPS_MAX) return ID_E_ARG;
        if (me.uid != 0) return ID_E_PERM;
        uint32_t tmp[ID_NGROUPS_MAX];
        if (n && (!b || user_copy_from(tmp, (const void *)b, (uint64_t)n * 4) < 0))
            return ID_E_ARG;
        return vfs_cred_groups_set(p->pid, tmp, n) < 0 ? ID_E_PERM : 0;
    }

    case SYS_SETSESSION:
        if (!p) return ID_E_NOPROC;
        if (me.uid != 0) return ID_E_PERM;
        return vfs_cred_session_set((uint32_t)a, (uint32_t)b) < 0 ? ID_E_PERM : 0;

    case SYS_GETSESSION: {
        uint32_t su, sg;
        vfs_cred_session(&su, &sg);
        return a == 1 ? (long)sg : (long)su;
    }
    }
    return ID_E_ARG;
}

/* --- /dev/vfsmeta rendering ---------------------------------------------- */

static int put(char *b, int max, int n, const char *s)
{ for (int i = 0; s[i] && n < max; i++) b[n++] = s[i]; return n; }
static int putn(char *b, int max, int n, unsigned long v)
{
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && n < max) b[n++] = t[--k];
    return n;
}

int vfs_cred_render(char *buf, int max)
{
    int n = put(buf, max, 0, "# session uid gid\n");
    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    n = put(buf, max, n, "session ");
    n = putn(buf, max, n, g_sess_uid);
    n = put(buf, max, n, " ");
    n = putn(buf, max, n, g_sess_gid);
    n = put(buf, max, n, "\n# pid uid gid groups\n");
    for (int i = 0; i < NCRED; i++) {
        if (!creds[i].pid) continue;
        n = putn(buf, max, n, (unsigned long)creds[i].pid);
        n = put(buf, max, n, " ");
        n = putn(buf, max, n, creds[i].uid);
        n = put(buf, max, n, " ");
        n = putn(buf, max, n, creds[i].gid);
        for (int g = 0; g < creds[i].ngroups; g++) {
            n = put(buf, max, n, g ? "," : " ");
            n = putn(buf, max, n, creds[i].groups[g]);
        }
        n = put(buf, max, n, "\n");
    }
    spin_unlock_irqrestore(&g_cred_lock, fl);
    if (n < max) buf[n] = 0;
    return n;
}
