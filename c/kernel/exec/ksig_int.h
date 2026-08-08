#ifndef LOGIT_KSIG_INT_H
#define LOGIT_KSIG_INT_H

/* Private to the two ksignal TUs. c/kernel/exec/ksignal.c owns the table and
 * the bookkeeping; c/kernel/exec/ksigframe.c owns the frame that is pushed onto
 * a user stack. They are split because they are two different kinds of code --
 * one is a lock and a bitmask, the other is register-level ABI -- and because a
 * file that is only ever read for one of those reasons should not be the size
 * of both. */

#include <stdint.h>
#include "spinlock.h"
#include "logit_abi.h"
#include "proc.h"          /* NPROC: one signal slot per PCB slot, no more */

#define KSIG_NSIG LOGIT_NSIG

/* Default actions. */
#define DFL_TERM 0
#define DFL_IGN  1
#define DFL_STOP 2
#define DFL_CONT 3

struct sigst {
    int      pid;                        /* 0 = free */
    uint64_t handler[KSIG_NSIG];         /* 0 = SIG_DFL, 1 = SIG_IGN, else a ring-3 va */
    uint64_t hmask[KSIG_NSIG];           /* sa_mask */
    uint64_t restorer[KSIG_NSIG];        /* required; see logit_abi.h */
    uint32_t hflags[KSIG_NSIG];          /* LOGIT_SA_* */

    /* The three words ksig_interrupted() reads WITHOUT the lock. Volatile
     * because that read is deliberately unlocked (see the header): it is a
     * hint, and the cost of a stale one is one more pass round a wait loop.
     * `ignored` is a CACHE of "this signal's disposition does nothing", kept in
     * step at the one place a disposition changes, so the unlocked reader needs
     * to consult neither handler[] nor the default table. */
    volatile uint64_t pending;
    volatile uint64_t blocked;
    volatile uint64_t ignored;

    volatile int stopped;                /* SIGSTOP/SIGTSTP; cleared by SIGCONT */
    uint64_t alarm_at;                   /* timer_ms() deadline, 0 = no alarm */
    uint64_t suspend_mask;               /* sigsuspend's saved mask */
    int      in_suspend;

    /* The fault that raised a synchronous signal, captured at ksig_fault()
     * time and handed to the frame. NOT read from CR2 at delivery: the
     * delivery path itself writes user memory, which on this kernel can
     * demand-fault (usercopy resolves COW and anonymous pages), so by the time
     * the frame is built CR2 may describe the signal machinery's own fault
     * rather than the program's. */
    uint64_t fault_cr2, fault_err, fault_trapno;
};

extern spinlock_t g_sig_lock;
extern struct sigst g_sig[NPROC];

/* Both TUs need these. */
struct sigst *ksig_find_locked(int pid);
int  ksig_default_action(int signo);
void ksig_recalc_ignored(struct sigst *s);
void ksig_set_pending(struct sigst *s, uint64_t bits);
void ksig_clear_pending(struct sigst *s, uint64_t bits);

/* SYS_SIGQUERY counters, bumped from both TUs. */
extern volatile uint64_t g_sig_delivered, g_sig_returned, g_sig_posted;
extern volatile uint64_t g_sig_defaulted, g_sig_dropped, g_sig_fpusaved;

#endif /* LOGIT_KSIG_INT_H */
