/* <sys/resource.h>. See the header for which numbers are real. */
#include <sys/resource.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#define USER_STACK_BYTES (256ul * 4096ul)   /* CLI_STACK_PAGES, c/kernel/exec/exec.c */

static int limit_of(int resource, struct rlimit *r)
{
    switch (resource) {
    case RLIMIT_CPU:     r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;
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

/* Only a no-op set is honoured: asking for exactly the value getrlimit()
 * already reports succeeds (a program that reads-then-writes back its own
 * limit, which autoconf-generated code routinely does, must not fail); asking
 * for anything else is refused, because nothing downstream would enforce it. */
int setrlimit(int resource, const struct rlimit *rlim)
{
    struct rlimit cur;
    if (!rlim) { errno = EFAULT; return -1; }
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
    /* Every field is 0: see the header note -- nothing on this kernel sums
     * CPU time, faults, or block I/O per process yet. */
    memset(usage, 0, sizeof *usage);
    return 0;
}
