/* <sys/resource.h>. See the header for which numbers are real. */
#include <sys/resource.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "logit_abi.h"

#define USER_STACK_BYTES (256ul * 4096ul)   /* CLI_STACK_PAGES, c/kernel/exec/exec.c */

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

static int limit_of(int resource, struct rlimit *r)
{
    switch (resource) {
    case RLIMIT_CPU: {
        /* The one rlimit this kernel actually enforces (c/kernel/sched/
         * sched.c: a per-thread ns budget, checked every 100 Hz tick).
         * RUCTL_GET_LIMIT_S returns -1 for "no cap", the same sentinel
         * RLIM_INFINITY already is once cast down through rlim_t. */
        long s = sys(SYS_RUSAGE, RUCTL_GET_LIMIT_S, 0, 0);
        r->rlim_cur = r->rlim_max = (s < 0) ? RLIM_INFINITY : (rlim_t)s;
        return 0;
    }
    case RLIMIT_FSIZE:   r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;
    case RLIMIT_DATA:    r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;   /* see <sys/mman.h> */
    case RLIMIT_STACK:   r->rlim_cur = r->rlim_max = USER_STACK_BYTES; return 0;
    case RLIMIT_CORE:    r->rlim_cur = r->rlim_max = 0; return 0;
    case RLIMIT_RSS:     r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;
    case RLIMIT_NPROC:   r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;
    case RLIMIT_NOFILE:  r->rlim_cur = r->rlim_max = OPEN_MAX; return 0;
    case RLIMIT_MEMLOCK: r->rlim_cur = r->rlim_max = 0; return 0;
    case RLIMIT_AS:      r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;
    default: errno = EINVAL; return -1;
    }
}

int getrlimit(int resource, struct rlimit *rlim)
{
    if (!rlim) { errno = EFAULT; return -1; }
    return limit_of(resource, rlim);
}

/* Only a no-op set is honoured for every resource EXCEPT RLIMIT_CPU: asking
 * for exactly the value getrlimit() already reports succeeds (a program that
 * reads-then-writes back its own limit, which autoconf-generated code
 * routinely does, must not fail); asking for anything else is refused,
 * because nothing downstream would enforce it.
 *
 * RLIMIT_CPU is real now (see the header and limit_of() above), so it takes
 * the actual value instead: seconds, or RLIM_INFINITY to clear the budget.
 * Both raising and lowering are allowed -- there is no fixed system ceiling
 * being loosened here, unlike RLIMIT_STACK/RLIMIT_NOFILE/etc, because the
 * budget is a number the caller is choosing for its OWN thread. rlim_max is
 * not tracked separately (this kernel has no privileged/unprivileged split on
 * it), so only rlim_cur is consulted. */
int setrlimit(int resource, const struct rlimit *rlim)
{
    struct rlimit cur;
    if (!rlim) { errno = EFAULT; return -1; }

    if (resource == RLIMIT_CPU) {
        /* rlim_t is unsigned; the syscall's "seconds" is signed with negative
         * meaning unlimited (see RUCTL_SET_LIMIT_S). RLIM_INFINITY (~0UL) is
         * exactly -1 reinterpreted, so it needs no special case -- and
         * anything else too large to be a signed 64-bit second count is, by
         * construction, far beyond the ~584-year point past which
         * sched_cpu_limit_set() itself already clamps to "no limit" (a value
         * that large cannot mean anything else), so letting the sign land
         * where it falls and trusting the kernel's own clamp is correct
         * rather than merely convenient. */
        long seconds = (long)rlim->rlim_cur;
        if (sys(SYS_RUSAGE, RUCTL_SET_LIMIT_S, seconds, 0) < 0) { errno = EINVAL; return -1; }
        return 0;
    }

    if (limit_of(resource, &cur) != 0) return -1;
    if (rlim->rlim_cur == cur.rlim_cur && rlim->rlim_max == cur.rlim_max) return 0;
    errno = EPERM;
    return -1;
}

int getrusage(int who, struct rusage *usage)
{
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD)
    { errno = EINVAL; return -1; }
    if (!usage) { errno = EFAULT; return -1; }
    memset(usage, 0, sizeof *usage);

    if (who == RUSAGE_CHILDREN) {
        /* A reaped child's CPU time is not retained anywhere on this kernel
         * (uthread_proc_reap() frees its threads' descriptors outright); an
         * honest zero rather than a fabricated number. */
        return 0;
    }

    /* RUSAGE_THREAD: the calling thread's own time. RUSAGE_SELF: that thread
     * plus every still-alive sibling of the same process -- see the long
     * comment on SYS_RUSAGE in include/abi/logit_abi.h for exactly what that
     * does and does not cover. For a single-threaded process (the ordinary
     * case here) the two calls return the same number. */
    long ns = sys(SYS_RUSAGE, RUCTL_GET_NS, who == RUSAGE_SELF ? 1 : 0, 0);
    if (ns < 0) return 0;   /* no accounting record: an honest zero, not a
                             * failure -- getrusage() does not fail for a
                             * live caller */

    /* All of it lands in ru_utime; see the header for why ru_stime stays 0
     * rather than a guessed split. */
    usage->ru_utime.tv_sec  = (long)(ns / 1000000000L);
    usage->ru_utime.tv_usec = (long)((ns % 1000000000L) / 1000L);
    return 0;
}
