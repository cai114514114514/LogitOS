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

/* Rendered for /dev/vfsmeta. Returns bytes written. */
int  vfs_cred_render(char *buf, int max);

#endif /* LOGIT_VFS_CRED_H */
