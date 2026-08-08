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
 * wide open. */

typedef unsigned long rlim_t;
#define RLIM_INFINITY  (~0UL)
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIM_SAVED_MAX RLIM_INFINITY

struct rlimit { rlim_t rlim_cur, rlim_max; };

#define RLIMIT_CPU     0   /* no CPU-time accounting per process: RLIM_INFINITY */
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

/* Every timeval-shaped field is 0: see CLAUDE.md's SYS_PROCS note, which
 * explains precisely why (the scheduler counts dispatches, never time, and
 * nothing sums memory per address space). A struct of honest zeros, not
 * invented ones. */
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

#endif /* _SYS_RESOURCE_H */
