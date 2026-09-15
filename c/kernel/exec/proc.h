#ifndef LOGIT_PROC_H
#define LOGIT_PROC_H

#include <stdint.h>
#include "../../../include/abi/aex_agent.h"
#include "spinlock.h"
#include "interrupts.h"   /* struct registers */

/* A process: an address space + a file-descriptor table + scheduling identity,
 * independent of any window. A GUI app is just a proc whose `gui` is non-NULL.
 * The shell, coreutils and forked children are procs with no window.          */
#define NPROC 32
/* Must equal c/apps/libc/include/limits.h's OPEN_MAX -- that header already
 * says so ("c/kernel/exec/proc.c fd table") and is not aspirational, so 16 here
 * was the stale side of the pair: a program trusting its own limits.h opened
 * fd 17 and got a plain failure, no ENFILE, no hint that the table it thinks
 * it has is twice the one it was given. Cost of raising it: `struct file
 * *fd[NFD]` gains 16 pointers (8 bytes each = 128 B) per proc; NPROC=32 slots
 * makes that 4096 B (4 KiB) of extra .bss total -- against a kheap arena
 * measured in tens of MiB (see the M "12 MiB program" section of CLAUDE.md),
 * this is noise. */
#define NFD   32

enum proc_state { PROC_FREE = 0, PROC_RUNNING, PROC_ZOMBIE, PROC_BUILDING };

struct file;   /* file.h */
struct aex_info;
struct elf_image;

struct proc {
    struct aex_agent_identity agent;
    int      pid, ppid;
    int      state;          /* enum proc_state */
    int      exit_code;
    int      tid;            /* scheduler thread id backing this proc */
    uint64_t cr3;            /* address space (PML4 phys) */
    void    *gui;            /* struct app* if it owns a window, else NULL */
    spinlock_t fd_lock;
    int execing;
    int system_service; /* Kernel-created credential root, set before publication. */
    int teardown; /* tid+1 elected under g_ut_lock; 0 until final teardown. */
    struct file *fd[NFD];
    char     cwd[128];
    char     name[32];

    /* M28 capabilities (docs/superpowers/specs/2026-08-14-m28-capabilities.md).
     * `caps` is a CAP_* bitmap (include/abi/logit_abi.h); `fs_prefix` further
     * scopes CAP_FS to paths under this subtree ("" = unscoped -- CAP_FS then
     * covers every path). Neither field is ever WIDENED after a process is
     * created: SYS_FORK copies both unchanged, SYS_EXECVE touches neither (see
     * the comment in proc_execve(), c/kernel/exec/exec.c), and the only way
     * either narrows is SYS_CAP_SPAWN creating a NEW process with its own,
     * separately ceiling-checked values (proc_cap_subset() in proc.c). There
     * is deliberately no "grant myself more" operation anywhere in this file.
     *
     * fs_prefix is NOT itself enforced here, or anywhere in this file -- see
     * the long comment above proc_cap_subset() in proc.c for exactly what this
     * field guarantees (a monotone-narrowing STRING relation between a parent
     * and a child's own bookkeeping) and what it does not (nothing about
     * whether a RESOLVED path, after symlinks, still falls under it -- that
     * question belongs to c/fs/vfs.c, a file this line does not own; see the
     * `not_done` note this change ships with). */
    unsigned long caps;
    char          fs_prefix[64];
};

void          proc_init(void);
long proc_agent_spawn(struct registers *r);
void proc_agent_bind_image(struct proc *, const struct elf_image *, unsigned);
int proc_agent_identity(int pid, struct aex_agent_identity *);
int proc_agent_channel_valid(int pid, uint64_t generation);
int proc_fd_take_exclusive(struct proc *, int fd, struct file *expected);
/* Register a proc for an address space about to run (called by wm_launch and by
 * the kernel `init` spawn). `gui` is the optional window owner. */
struct proc  *proc_create(uint64_t cr3, void *gui, const char *name, int ppid);
/* Scheduler calls after initialization, before the first runnable enqueue.
 * Existing RUNNING processes (thread clones) keep their original main tid. */
void proc_publish(struct proc *p, int tid);
void proc_abort_build(struct proc *p); /* caller already released the unpublished AS */
struct proc  *proc_current(void);
int proc_snapshot(int pid, struct proc *out);
int proc_exists(int pid);

/* fork the current process: clones the address space + fd table and creates a
 * child thread that resumes from the same syscall returning 0. Returns the
 * child pid to the parent. `r` is the parent's int 0x80 register frame. */
long          proc_fork(struct registers *r, const void *user_fxarea);

void proc_kill_check(void);
void          proc_exit(int code);                  /* never returns */
long          proc_execve(struct registers *r);     /* replace the user address space (exec.c) */
int           proc_exec_space(struct proc *p, uint64_t old, uint64_t next);
struct elf_image;
uint64_t      exec_interpreter_stack(uint64_t cr3, const struct elf_image *img, const char *path, const char *arg, int pages);
int           proc_spawn(const char *path, char **argv);  /* init: launch a CLI proc on the tty (exec.c) */
int           proc_spawn_service(const char *path, char **argv);
/* Every site that loads a program image calls this with what the loader
 * reported, so the "how many pages came from the page cache" line in
 * exec_report() covers the DESKTOP's launches too and not only the shell's.
 * It lives in exec.c because a boot-lifetime counter inside elf.c would be a
 * global surviving between cases in the host loader tests. */
struct elf_image;
void          exec_note_load(const char *what, const struct elf_image *ei);
long          proc_cap_spawn(struct registers *r);  /* M28: SYS_CAP_SPAWN -- fork+exec a capability-
                                                      * bounded child (exec.c). Returns the child pid,
                                                      * or a negative LOGIT_CAP_E_* code. */
/* reap a zombie child. `options` is the SYS_WAITPID third argument (WNOHANG,
 * bit 1, matching c/apps/libc/include/sys/wait.h -- this header cannot include
 * that one, so the bit is a literal known to agree with it, same shape as
 * SYS_KILL's LOGIT_KILL_SIGNAL flag in logit_abi.h). Any other bit is refused
 * (SIG_E_NOSYS) rather than silently ignored -- see proc.c. */
long          proc_waitpid(int pid, int *status, int options);
void          proc_reap(void);                      /* free orphan/GUI zombies (WM loop) */

/* fd table (P2). */
int           proc_fd_alloc(struct proc *p, struct file *f);  /* lowest free fd, or -1 */
struct file  *proc_fd_acquire(struct proc *p, int fd); /* caller must file_close */
int proc_fd_close(struct proc *p, int fd); /* -1 bad fd, -2 failed final flush */
int proc_fd_close_if(struct proc *p, int fd, struct file *expected);
int proc_fd_dup2(struct proc *p, int old, int replacement);
int proc_fd_pair(struct proc *p, struct file *a, struct file *b, int out[2]);
void proc_fd_clone(struct proc *dst, struct proc *src);
void proc_fd_close_all(struct proc *p);
/* Resolve `in` to an absolute canonical path against p->cwd (collapses . and ..). */
void          proc_resolve(struct proc *p, const char *in, char *out, int max);

/* M28: is (req_caps, req_prefix) an ALLOWED narrowing of (cur_caps,
 * cur_prefix)? See the long comment above this function's definition in
 * proc.c for the two conditions and what this deliberately does not check. */
int           proc_cap_subset(unsigned long req_caps, const char *req_prefix,
                               unsigned long cur_caps, const char *cur_prefix);

#endif /* LOGIT_PROC_H */
