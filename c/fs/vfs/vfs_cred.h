#ifndef LOGIT_VFS_CRED_H
#define LOGIT_VFS_CRED_H

#include "vfs_meta.h"   /* struct vcred */

/* ---------------------------------------------------------------------------
 * Process credentials.
 *
 * WHY THEY ARE HERE AND NOT IN struct proc
 * ----------------------------------------|
 * They belong in `struct proc`, next to pid/ppid/cwd, and that is where they
 * should end up. c/kernel/exec/proc.c is owned by another line right now, so
 * this keeps a table on the side keyed by pid, and derives a process's
 * credential from its PARENT when it has no entry of its own. That is not a
 * trick: it produces exactly the inheritance a uid in the PCB would, because
 * fork and execve both leave the credential alone, and a child with no entry
 * IS its parent's credential. The cost is a walk up the ppid chain (bounded by
 * NPROC) instead of a struct field, and the fact that a pid must be reachable
 * through proc_by_pid() for the walk to work -- which is true for exactly as
 * long as the process is alive, which is exactly when it matters.
 *
 * WHAT proc.c SHOULD ADD, when its owner is ready:
 *     struct proc { ... uint32_t uid, gid; ... };
 *   - alloc_proc(): uid = gid = 0
 *   - proc_fork():  child->uid = parent->uid; child->gid = parent->gid
 *   - proc_execve(): unchanged (a credential survives exec)
 * and then vfs_cred_current() becomes two field reads and this file's table
 * and its pid sweep go away. Nothing else in the VFS changes.
 * ------------------------------------------------------------------------ */

/* The credential of the process running right now. Kernel threads and early
 * boot -- anything with no current process -- are root, which is what makes
 * mounting the root filesystem and loading fonts work before there is a
 * userland to have a uid. */
void vfs_cred_current(struct vcred *c);

/* Read/replace a specific process's credential. `vfs_cred_set` applies the
 * POSIX rule and nothing more: root may become anybody, anybody else may not
 * change uid at all. Returns 0 or a negative VFS_E*. */
int  vfs_cred_get(int pid, struct vcred *c);
int  vfs_cred_set(int pid, uint32_t uid, uint32_t gid);

/* Supplementary groups. Stored per process and inherited down the same ppid
 * chain the uid is. `vfs_cred_groups_get` returns the COUNT (which may exceed
 * `max`; only `max` are written). Both are root-only, like the uid. */
int  vfs_cred_groups_get(int pid, uint32_t *out, int max);
int  vfs_cred_groups_set(int pid, const uint32_t *list, int n);

/* Is the CURRENT process a member of `gid` through its supplementary set?
 * c/fs/vfs_meta.c calls this through a WEAK declaration so that the host VFS
 * unit test, which does not link this file, resolves it to NULL and behaves
 * exactly as it did before groups existed. */
int  vfs_cred_ingroup(uint32_t gid);

/* THE LOGIN SESSION: the credential of a process that was never told who it
 * belongs to. See the SYS_SETSESSION comment in include/abi/logit_abi.h for
 * the whole argument; in one line, the window manager brings a desktop up
 * before anything has authenticated and those processes have no parent to
 * inherit from. Before a login it is root, which is the boot state.
 *
 * vfs_cred_session_set is root-only and sets the session AND the caller's own
 * credential together, because /bin/login cannot do the two separately in
 * either order -- see the comment on the function. */
void vfs_cred_session(uint32_t *uid, uint32_t *gid);
int  vfs_cred_session_set(uint32_t uid, uint32_t gid);

/* The kernel side of SYS_GETUID..SYS_GETSESSION (150-159), forwarded whole
 * from c/kernel/exec/syscall.c. Returns the value for rax. */
long id_syscall(long num, long a, long b, long c);

/* Rendered for /dev/vfsmeta. Returns bytes written. */
int  vfs_cred_render(char *buf, int max);

#endif /* LOGIT_VFS_CRED_H */
