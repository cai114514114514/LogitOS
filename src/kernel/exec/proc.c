#include <stdint.h>
#include <stddef.h>
#include "proc.h"
#include "file.h"
#include "sched.h"
#include "vmm.h"
#include "kprintf.h"

void wm_app_exit(void);   /* wm.c: mark the current proc's window dead */

static struct proc procs[NPROC];
static int next_pid = 1;

void proc_init(void)
{
    for (int i = 0; i < NPROC; i++) { procs[i].state = PROC_FREE; procs[i].pid = 0; }
}

struct proc *proc_current(void) { return (struct proc *)sched_current_data(); }

struct proc *proc_by_pid(int pid)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != PROC_FREE && procs[i].pid == pid) return &procs[i];
    return NULL;
}

static struct proc *alloc_proc(void)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state == PROC_FREE) {
            struct proc *p = &procs[i];
            for (int f = 0; f < NFD; f++) p->fd[f] = NULL;
            p->state = PROC_RUNNING;
            p->pid = next_pid++;
            p->ppid = 0; p->exit_code = 0; p->tid = -1; p->cr3 = 0; p->gui = NULL;
            p->cwd[0] = '/'; p->cwd[1] = 0; p->name[0] = 0;
            return p;
        }
    return NULL;
}

static void scopy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

struct proc *proc_create(uint64_t cr3, void *gui, const char *name, int ppid)
{
    struct proc *p = alloc_proc();
    if (!p) return NULL;
    p->cr3 = cr3; p->gui = gui; p->ppid = ppid;
    scopy(p->name, name, sizeof p->name);
    return p;
}

long proc_fork(struct registers *r)
{
    struct proc *parent = proc_current();
    if (!parent) return -1;

    uint64_t space = vmm_new_space();
    if (!space) return -1;
    vmm_clone_user(space, parent->cr3);     /* eager copy of the user subtree */

    struct proc *child = alloc_proc();
    if (!child) { vmm_free_space(space); return -1; }
    child->cr3  = space;
    child->ppid = parent->pid;
    child->gui  = NULL;                      /* a forked child has no window */
    scopy(child->name, parent->name, sizeof child->name);
    scopy(child->cwd, parent->cwd, sizeof child->cwd);
    for (int i = 0; i < NFD; i++) {
        child->fd[i] = parent->fd[i];
        if (child->fd[i]) file_dup(child->fd[i]);
    }

    child->tid = thread_fork(child->name, r, child, space);
    return child->pid;                       /* parent sees the child's pid */
}

void proc_exit(int code)
{
    struct proc *p = proc_current();
    if (p) {
        p->exit_code = code;
        for (int i = 0; i < NFD; i++)
            if (p->fd[i]) { file_close(p->fd[i]); p->fd[i] = NULL; }
        p->state = PROC_ZOMBIE;
    }
    wm_app_exit();        /* if this proc owns a window, mark it dead (no-op otherwise) */
    thread_exit();        /* leaves the ring; never returns. Address space freed by
                           * proc_waitpid (parent) or proc_reap (orphan/GUI). */
}

/* Reap one zombie child of the current process. Blocks (cooperatively) until a
 * matching child is a zombie. pid == -1 waits for any child. */
long proc_waitpid(int pid, int *status)
{
    struct proc *self = proc_current();
    if (!self) return -1;

    for (;;) {
        int have_child = 0;
        for (int i = 0; i < NPROC; i++) {
            struct proc *c = &procs[i];
            if (c->state == PROC_FREE || c->ppid != self->pid) continue;
            if (pid != -1 && c->pid != pid) continue;
            have_child = 1;
            if (c->state == PROC_ZOMBIE) {
                int rpid = c->pid, code = c->exit_code;
                if (c->cr3) vmm_free_space(c->cr3);
                c->state = PROC_FREE; c->pid = 0; c->cr3 = 0;
                if (status) *status = code;
                return rpid;
            }
        }
        if (!have_child) return -1;
        schedule();        /* let the child run, then re-check */
    }
}

/* Free zombies that nobody will waitpid() for: GUI apps (ppid 0, "parent" is the
 * WM) and orphans whose parent slot is already gone. Called from the WM loop. */
void proc_reap(void)
{
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state != PROC_ZOMBIE) continue;
        struct proc *par = p->ppid ? proc_by_pid(p->ppid) : NULL;
        if (p->ppid == 0 || !par) {            /* no live waiter -> free it */
            if (p->cr3) vmm_free_space(p->cr3);
            p->state = PROC_FREE; p->pid = 0; p->cr3 = 0;
        }
    }
}
