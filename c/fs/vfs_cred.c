/* Process credentials for the VFS. See vfs_cred.h for why they live here
 * rather than in struct proc, and what proc.c should grow to take them back. */

#include <stdint.h>
#include <stddef.h>
#include "vfs_cred.h"
#include "vfs_path.h"
#include "proc.h"
#include "spinlock.h"

#define NCRED 32

struct centry {
    int      pid;          /* 0 = free */
    uint32_t uid, gid;
};

static struct centry creds[NCRED];
static spinlock_t g_cred_lock = SPINLOCK_INIT;

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

int vfs_cred_get(int pid, struct vcred *c)
{
    if (!c) return VFS_EINVAL;
    c->uid = 0; c->gid = 0;
    if (pid <= 0) return 0;

    /* Walk up the ppid chain to the first process that has an entry. A process
     * with no entry anywhere in its ancestry is root, which is the boot state.
     * Bounded by NPROC so a corrupted ppid cycle cannot spin here. */
    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    int cur = pid;
    for (int hop = 0; hop < NPROC && cur > 0; hop++) {
        struct centry *e = find_locked(cur);
        if (e) { c->uid = e->uid; c->gid = e->gid; break; }
        spin_unlock_irqrestore(&g_cred_lock, fl);
        struct proc *p = proc_by_pid(cur);           /* takes its own lock */
        fl = spin_lock_irqsave(&g_cred_lock);
        if (!p) break;
        cur = p->ppid;
    }
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
        e->pid = pid;
    }
    e->uid = uid; e->gid = gid;
    spin_unlock_irqrestore(&g_cred_lock, fl);
    return 0;
}

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
    int n = put(buf, max, 0, "# pid uid gid\n");
    uint64_t fl = spin_lock_irqsave(&g_cred_lock);
    for (int i = 0; i < NCRED; i++) {
        if (!creds[i].pid) continue;
        n = putn(buf, max, n, (unsigned long)creds[i].pid);
        n = put(buf, max, n, " ");
        n = putn(buf, max, n, creds[i].uid);
        n = put(buf, max, n, " ");
        n = putn(buf, max, n, creds[i].gid);
        n = put(buf, max, n, "\n");
    }
    spin_unlock_irqrestore(&g_cred_lock, fl);
    if (n < max) buf[n] = 0;
    return n;
}
