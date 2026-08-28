#ifndef LOGIT_UNIXSTUB_WAIT_H
#define LOGIT_UNIXSTUB_WAIT_H

/* Host stub for c/kernel/core/wait.h, so c/net/core/unix.c compiles into the
 * white-box gate (tests/unit/unix_test.c). Path-qualified in unix.c for the
 * reason that file states, which is why this sits under a `kernel/core/`
 * subdirectory rather than being a flat wait.h.
 *
 * THIS IS NOT tests/unit/tcpstub's wait.h AND IT MUST NOT BE. That one
 * evaluates the condition once and returns, and says so: tcp_test.c IS the
 * peer, it calls tcp_input() itself from the same thread, so there is nothing
 * to wait for. AF_UNIX is the opposite shape -- every blocking case in
 * unix_test.c is "this end parks and the OTHER end then acts", and a
 * once-and-return stub would make every one of those cases pass without the
 * park ever happening. `block: close -> EOF` would read 0 whether or not
 * unix_read() blocks, so an implementation that never blocks at all would score
 * green on the whole of t_block.
 *
 * SO THE PARK IS MODELLED, in three parts:
 *
 *   1. ustub_on_park  is the other process. The test installs a callback and
 *      the stub calls it AT THE POINT THE THREAD WOULD HAVE PARKED -- so
 *      "the peer closed while we were blocked in read()" becomes a thing that
 *      happens in a defined order rather than a race.
 *
 *   2. ustub_parks    counts real parks (the condition was false on entry).
 *      Every blocking check in unix_test.c asserts this went up, which is what
 *      separates "blocked, then was woken with the right answer" from "returned
 *      the right answer immediately".
 *
 *   3. THE WAKE IS LOAD-BEARING, and this is the part a naive stub gets wrong.
 *      A parked thread is not runnable until somebody calls sched_wake(); it
 *      does NOT re-test its predicate on its own. So this stub re-tests only if
 *      the queue's wake counter MOVED while the hook ran. A hook that makes the
 *      predicate true and forgets waitq_wake_all() is a LOST WAKEUP -- a hang on
 *      the real machine -- and here it is ustub_stuck(): one FAIL line naming
 *      the wait, and exit 1.
 *
 *      That is what makes UNIX_NEGCTL_NOWAKE a control at all. With that macro
 *      unix.c's unix_wake() is a no-op, unix_release() still sets `gone`, and a
 *      stub that simply re-evaluated the predicate would sail through and the
 *      "control" would prove nothing. Measured: it reddens exactly 1, and that 1
 *      is this abort.
 *
 *      AND THAT WAS MEASURED BOTH WAYS, because it is the one design decision
 *      in this file that a reader would otherwise take on trust. The same
 *      stub with the wake check replaced by "re-test the predicate after the
 *      hook" was built and run on 2026-08-28: `-DUNIX_NEGCTL_NOWAKE` then
 *      prints `unix: 132/132 checks` and EXITS 0. The gate looks perfect and
 *      one of its three controls is dead, which is the failure mode
 *      CLAUDE.md's test-suite section calls worse than having no control --
 *      it reads like one. The wake counter is what stops that.
 *
 * WHY ustub_stuck() FLUSHES STDOUT FIRST. The checks print to buffered stdout
 * and this line goes to unbuffered stderr; both land in one log that the
 * negative controls count with `grep -c '^FAIL'`. Without the flush the abort
 * is spliced into the middle of a half-written check line and the two count as
 * one, so UNIX_NEGCTL_ONEDIR alternates between 17 and 18 run to run -- a gate
 * that fails intermittently while pointing at AF_UNIX. See the counts table in
 * tests/net.mk.
 *
 * WHAT THIS STUB THEREFORE DOES NOT COVER: nothing here is concurrent. There is
 * one thread, the lock is not a lock, and the lost-wakeup window rule 2 of the
 * real header exists to close (evaluate the predicate under the same lock the
 * sleep is handed) cannot be violated because there is no second core to
 * violate it from. The multi-thread half is the device's to prove, not this
 * file's -- said out loud because a stub that quietly no-ops a primitive is what
 * makes a suite look like it covers more than it does. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Defined in tests/unit/unix_test.c, ahead of its `#include "unix.c"`. */
extern long   ustub_parks;
extern long   ustub_wakes;
extern void (*ustub_on_park)(void *);
extern void  *ustub_park_arg;
extern const char *ustub_where;      /* names the wait under test, for the abort */

/* --------------------------------------------------------------- the queue */

typedef int spinlock_t;
#define SPINLOCK_INIT 0

struct waiter { int unused; };

struct waitq {
    spinlock_t     lock;
    struct waiter *head, *tail;
    unsigned long  wakes;
};
#define WAITQ_INIT { 0, 0, 0, 0 }

static inline void waitq_init(struct waitq *q)
{ q->lock = 0; q->head = q->tail = 0; q->wakes = 0; }

/* PER-QUEUE, not just the global counter: wait_event below asks "did THIS
 * queue get woken", and a global would let a wake posted on an unrelated
 * socket's queue release a sleeper that nobody woke. */
static inline int waitq_wake_one(struct waitq *q)
{ q->wakes++; ustub_wakes++; return 0; }
static inline int waitq_wake_all(struct waitq *q)
{ q->wakes++; ustub_wakes++; return 0; }

static inline void ustub_stuck(const char *cond)
{
    fflush(stdout);                   /* see the header comment -- not cosmetic */
    fprintf(stderr,
            "FAIL: stub: nothing woke the sleeper -- %s (waiting for: %s)\n",
            ustub_where ? ustub_where : "(unnamed wait)", cond);
    fflush(stderr);
    exit(1);
}

/* Park until `cond` holds. The hook stands in for the other process; the wake
 * counter stands in for the scheduler. Deliberately NOT a loop that re-tests on
 * its own -- see part 3 of the header comment. */
#define wait_event(q, cond)                                                    \
    do {                                                                       \
        struct waitq *__q = (q);                                               \
        while (!(cond)) {                                                      \
            unsigned long __w0 = __q->wakes;                                   \
            ustub_parks++;                                                     \
            if (ustub_on_park) ustub_on_park(ustub_park_arg);                  \
            if (__q->wakes == __w0) ustub_stuck(#cond);                        \
        }                                                                      \
    } while (0)

/* wait_event_timeout() is deliberately absent. unix.c has no bounded wait --
 * every park in it is "until the peer acts or a signal arrives" -- so a stub
 * for it here would have to invent a timeout policy with no clock to measure
 * it against, and the first caller would inherit that invention. Add it when
 * something in unix.c needs one, and give it a real deadline then. */

/* ------------------------------------------------------- signals, and EINTR */

/* unix.c declares these WEAK and treats NULL as "this build has no signals"
 * (sig_interrupted() folds a null ksig_interrupted to 0). That idiom is ELF's:
 * an undefined weak symbol resolves to NULL there. On the documented dev host
 * -- macOS / Apple Silicon, Mach-O -- it does not resolve at all, it is a hard
 * link error:
 *
 *     Undefined symbols for architecture arm64:
 *       "_ksig_interrupted", referenced from: ...
 *
 * so the gate cannot be linked at all on the machine it is meant to be run on.
 * Supplying WEAK DEFINITIONS here is the stub's job rather than a workaround:
 * signal delivery is a kernel service exactly like the heap and the scheduler,
 * and this directory is where this gate's kernel is. They are weak so that a
 * kernel-side change to the declaration idiom (a portable weak macro, say)
 * links against either.
 *
 * NEITHER IS A LIE ABOUT BEHAVIOUR, which is the bar for a stub in this tree:
 *   - ksig_interrupted() returns 0 always. Identical to the NULL branch, so
 *     every `|| sig_interrupted()` in unix.c stays false and the SIG_E_INTR
 *     paths remain UNTESTED here -- as they were, and as they must be, because
 *     there is no signal to deliver and no process to deliver it to. A stub
 *     that returned 1 sometimes would turn every blocking check into a
 *     coin toss.
 *   - ksig_post_current() records the signal and returns 0. It does NOT make
 *     the write succeed or fail differently: unix_write() returns
 *     `sent > 0 ? sent : -1` regardless, so `block: write to dead peer` reads
 *     -1 whether this function exists or is NULL. The count is here so the
 *     SIGPIPE is observable rather than swallowed.
 *
 * The two counters below are DEFINITIONS in a header, which is legal here for
 * the same reason the weak function definitions are: exactly one translation
 * unit includes this file, because exactly one file #includes unix.c. They are
 * non-static so a future check in unix_test.c can `extern` them and assert on
 * the SIGPIPE rather than merely on the -1. Nothing reads them today.
 *
 * AND THIS BLOCK STANDS ASIDE THE DAY unix.c IS CONVERTED. include/weaksym.h
 * is the tree's portable spelling of the same idiom and it works by emitting a
 * WEAK DEFINITION per site (LOGIT_WEAK_STUB, a `.weak_definition` in
 * __TEXT,__lgtweak). Two weak definitions of _ksig_interrupted in ONE object
 * file is an assembler error, not a silent win, so the two mechanisms must not
 * both fire. The guard is "has weaksym.h been included by the time unix.c
 * reaches us" -- true only in a converted tree, where its stubs own the
 * symbols and this block would be the duplicate.
 *
 * If a conversion puts that #include AFTER "kernel/core/wait.h" the guard
 * cannot see it and the build fails loudly on the redefinition, naming the
 * symbol. That is the right way round: delete this block then, do not
 * reorder unix.c to suit a test stub. Note also that unix.c:585's guard is a
 * bare `if (ksig_post_current)`, which weaksym.h documents as always-true on
 * Mach-O -- a conversion that changes the declaration and not the guard turns
 * `block: write to dead peer` into a SIGTRAP. */
#ifndef LOGIT_WEAKSYM_H

long ustub_sigpipes  = 0;    /* SIGPIPEs unix_write() posted */
int  ustub_last_signal = 0;  /* the last signo, so it is not merely swallowed */

__attribute__((weak)) int ksig_post_current(int signo)
{
    ustub_last_signal = signo;
    ustub_sigpipes++;
    return 0;
}

__attribute__((weak)) int ksig_interrupted(void) { return 0; }

#endif /* !LOGIT_WEAKSYM_H */

#endif /* LOGIT_UNIXSTUB_WAIT_H */
