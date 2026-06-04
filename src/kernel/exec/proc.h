#ifndef AQUA_PROC_H
#define AQUA_PROC_H

#include <stdint.h>
#include "interrupts.h"   /* struct registers */

/* A process: an address space + a file-descriptor table + scheduling identity,
 * independent of any window. A GUI app is just a proc whose `gui` is non-NULL.
 * The shell, coreutils and forked children are procs with no window.          */
#define NPROC 32
#define NFD   16

enum proc_state { PROC_FREE = 0, PROC_RUNNING, PROC_ZOMBIE };

struct file;   /* file.h */

struct proc {
    int      pid, ppid;
    int      state;          /* enum proc_state */
    int      exit_code;
    int      tid;            /* scheduler thread id backing this proc */
    uint64_t cr3;            /* address space (PML4 phys) */
    void    *gui;            /* struct app* if it owns a window, else NULL */
    struct file *fd[NFD];
    char     cwd[128];
    char     name[32];
};

void          proc_init(void);
/* Register a proc for an address space about to run (called by wm_launch and by
 * the kernel `init` spawn). `gui` is the optional window owner. */
struct proc  *proc_create(uint64_t cr3, void *gui, const char *name, int ppid);
struct proc  *proc_current(void);
struct proc  *proc_by_pid(int pid);

/* fork the current process: clones the address space + fd table and creates a
 * child thread that resumes from the same syscall returning 0. Returns the
 * child pid to the parent. `r` is the parent's int 0x80 register frame. */
long          proc_fork(struct registers *r);

void          proc_exit(int code);                  /* never returns */
long          proc_execve(struct registers *r);     /* replace the user address space (exec.c) */
int           proc_spawn(const char *path, char **argv);  /* init: launch a CLI proc on the tty (exec.c) */
long          proc_waitpid(int pid, int *status);   /* reap a zombie child */
void          proc_reap(void);                      /* free orphan/GUI zombies (WM loop) */

/* fd table (P2). */
int           proc_fd_alloc(struct proc *p, struct file *f);  /* lowest free fd, or -1 */
struct file  *proc_fd_get(struct proc *p, int fd);
/* Resolve `in` to an absolute canonical path against p->cwd (collapses . and ..). */
void          proc_resolve(struct proc *p, const char *in, char *out, int max);

#endif /* AQUA_PROC_H */
