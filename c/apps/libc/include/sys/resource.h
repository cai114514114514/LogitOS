#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H
#include <sys/time.h>

/* Resource limits/usage on a kernel that enforces almost none of them.
 * <limits.h> already states the real, fixed bounds this system has (OPEN_MAX,
 * PIPE_BUF, ...); this header exposes the ones a program can query as rlimits,
 * and is honest that they cannot be RAISED (there is nowhere to raise them
 * to -- OPEN_MAX is literally sizeof proc->fd[]) and mostly cannot be LOWERED
 * either (nothing in c/kernel/exec checks a per-process limit before acting).
 * getrlimit() is real. setrlimit() only succeeds for a limit that is already
 * true, so a program cannot be told it tightened something that is still
 * wide open.
 *
 * RLIMIT_CPU IS THE ONE EXCEPTION (2026-08-20), and it is a real exception,
 * not a loophole in that rule: c/kernel/sched/sched.c now tracks actual CPU
 * time per thread and enforces a budget against it (SYS_RUSAGE), so
 * setrlimit(RLIMIT_CPU, ...) sets something a downstream check genuinely
 * consults, both raising and lowering, rather than only accepting a no-op.
 * Every other resource below is unchanged. */

typedef unsigned long rlim_t;
#define RLIM_INFINITY  (~0UL)
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIM_SAVED_MAX RLIM_INFINITY

struct rlimit { rlim_t rlim_cur, rlim_max; };

#define RLIMIT_CPU     0   /* real: a per-THREAD ns budget enforced from the timer
                            * tick (c/kernel/sched/sched.c, SYS_RUSAGE). Nominally a
                            * per-PROCESS limit in POSIX; this kernel mostly runs one
                            * thread per process (M30 pthreads are the exception), so
                            * the two coincide in the ordinary case -- see the long
                            * comment on SYS_RUSAGE in include/abi/logit_abi.h for
                            * exactly what a multi-threaded process gets instead. */
#define RLIMIT_FSIZE   1   /* no per-file size cap: RLIM_INFINITY */
#define RLIMIT_DATA    2   /* the mmap arena reservation -- see <sys/mman.h> */
#define RLIMIT_STACK   3   /* the fixed 1 MiB user stack execve() builds
                            * (CLI_STACK_PAGES, c/kernel/exec/exec.c) -- fixed,
                            * not a default that can be raised */
#define RLIMIT_CORE    4   /* no core dumps exist: always 0 */
#define RLIMIT_RSS     5   /* not tracked per process: RLIM_INFINITY */
#define RLIMIT_NPROC   6   /* no per-user process cap: RLIM_INFINITY */
#define RLIMIT_NOFILE  7   /* OPEN_MAX -- real, and it is the fd table size */
#define RLIMIT_MEMLOCK 8   /* mlock() always fails (see <sys/mman.h>): 0 */
#define RLIMIT_AS      9   /* same as RLIMIT_DATA on this system */
#define RLIMIT_NLIMITS 10

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

/* ru_utime is now real (2026-08-20): the calling thread's own CPU time
 * (RUSAGE_THREAD), or that thread plus every still-alive sibling of the same
 * process (RUSAGE_SELF) -- see getrusage() below and the long comment on
 * SYS_RUSAGE in include/abi/logit_abi.h. RUSAGE_CHILDREN and every other
 * field are still honest zeros: nothing on this kernel retains a reaped
 * child's CPU time or sums memory per address space, and CLAUDE.md's
 * SYS_PROCS note explains why memory in particular is a harder problem than
 * this change takes on. ru_stime is also zero, and for a narrower, stated
 * reason: this kernel does not yet split ring-3 from ring-0 time PER THREAD
 * (that needs instrumentation at every ring-0/ring-3 crossing in
 * c/kernel/cpu/interrupts.c, a file this change does not own), so the whole
 * measured total is reported as user time rather than a guessed split. */
struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals, ru_nvcsw, ru_nivcsw;
};

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getrusage(int who, struct rusage *usage);

/* --- process priority (2026-08-20) -----------------------------------------
 * REAL, and it is worth saying so beside the paragraph above that lists what
 * is not: c/kernel/sched/sched.c weights its pick loop by nice, so a value set
 * here changes which thread the scheduler dispatches, measurably (make
 * test-sched). This is the second thing in this header that a downstream check
 * genuinely consults, after RLIMIT_CPU.
 *
 * PRIO_PGRP and PRIO_USER are DECLARED because a ported program writes
 * PRIO_PROCESS by name and needs the others to exist for its switch statement
 * to compile -- and they are REFUSED with EINVAL at runtime, because this
 * kernel has no process groups and no per-user process list. Accepting either
 * would renice one process while the caller believed it had reniced a group.
 *
 * getpriority() returns -1 for the legal nice value -1, so POSIX's rule
 * applies: clear errno, call, then inspect errno. See the implementation note
 * in c/apps/libc/src/resource.c. */
#ifndef __LOGIT_ID_T_DEFINED
#define __LOGIT_ID_T_DEFINED
typedef int id_t;
#endif

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

#define PRIO_MIN (-20)
#define PRIO_MAX   20   /* exclusive upper bound, as on Linux: the highest
                         * settable nice is 19. Kept at 20 rather than 19 so a
                         * program that loops `for (n = PRIO_MIN; n < PRIO_MAX;
                         * n++)` covers exactly the real range. */

int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

#endif /* _SYS_RESOURCE_H */
