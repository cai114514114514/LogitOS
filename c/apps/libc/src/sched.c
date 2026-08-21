/* <sched.h>. See the header for what is real: yield and the current CPU
 * index. Everything about policy/priority is refused honestly -- see below. */
#include <sched.h>
#include <errno.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

int sched_yield(void) { sys(SYS_YIELD, 0, 0, 0); return 0; }

int sched_getcpu(void)
{
    long r = sys(SYS_CPU_INDEX, 0, 0, 0);
    if (r < 0) { errno = ENOSYS; return -1; }
    return (int)r;
}

/* SCHED_OTHER is the only policy that exists, and its REALTIME priority range
 * is degenerately [0,0] -- a real answer, not an arbitrary one. Any other
 * policy is refused: reporting a range for FIFO/RR would tell a caller those
 * policies can be requested, and they cannot.
 *
 * This used to read "there is no priority scheduler at all", which stopped
 * being true on 2026-08-20 when the kernel's pick loop became nice-weighted.
 * The [0,0] answer does NOT change, and that is the point worth keeping: a
 * nice-weighted share is not a sched_param priority. Nice decides how big a
 * slice of a contended CPU a thread gets; sched_priority decides who runs
 * INSTEAD of whom, with no share for the loser at all, and nothing in
 * c/kernel/sched/sched.c does that. Reporting a non-zero range here because
 * nice exists would be the exact substitution this file refuses to make.
 * Nice lives in <sys/resource.h> and <unistd.h>, where POSIX puts it. */
int sched_get_priority_max(int policy)
{ if (policy != SCHED_OTHER) { errno = EINVAL; return -1; } return 0; }
int sched_get_priority_min(int policy)
{ if (policy != SCHED_OTHER) { errno = EINVAL; return -1; } return 0; }

int sched_getparam(int pid, struct sched_param *param)
{
    (void)pid;
    if (!param) { errno = EINVAL; return -1; }
    param->sched_priority = 0;
    return 0;
}
/* Setting any priority other than the only one that exists is refused rather
 * than silently accepted -- see the header note. */
int sched_setparam(int pid, const struct sched_param *param)
{
    (void)pid;
    if (!param || param->sched_priority != 0) { errno = EINVAL; return -1; }
    return 0;
}
int sched_getscheduler(int pid) { (void)pid; return SCHED_OTHER; }
int sched_setscheduler(int pid, int policy, const struct sched_param *param)
{
    (void)pid; (void)param;
    if (policy != SCHED_OTHER) { errno = EINVAL; return -1; }
    return 0;
}
