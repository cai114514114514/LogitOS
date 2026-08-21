#include "clib.h"

/* nice [-n INC] COMMAND [ARG...] -- run COMMAND with an adjusted nice value.
 * nice                            -- print the caller's current nice value.
 *
 * The consumer the weighted scheduler was built for. Everything about it turns
 * on ONE property of this kernel that is easy to miss: execve() replaces the
 * image IN THE SAME THREAD (c/kernel/exec/exec.c), so a nice value set here
 * survives into COMMAND with no code at all. There is no "pass the priority to
 * the new program" mechanism anywhere and none is needed -- which is also why
 * this is a wrapper rather than a shell builtin.
 *
 * DEFAULT INCREMENT 10, not 1, and matching POSIX. On this machine that is a
 * factor of exactly two in CPU share (the weight table is built so ten nice
 * levels are a doubling -- see include/abi/logit_abi.h), which is a decision a
 * person typing `nice make` can actually predict.
 *
 * NEGATIVE INCREMENTS ARE PASSED THROUGH AND MAY BE REFUSED. `nice -n -5` from
 * a non-root user gets SCHED_E_PERM from the kernel and this program says so
 * and REFUSES TO RUN THE COMMAND. The alternative -- warn and run at the old
 * priority, which is what GNU nice does -- is wrong here: a user who typed a
 * negative increment is asking for the job to be fast, and running it anyway
 * at normal priority looks like it worked. Erroring out is recoverable in one
 * keystroke; a job that quietly ran at the wrong priority for an hour is not.
 */

static void usage(void)
{
    errs("usage: nice [-n INC] COMMAND [ARG...]\n");
    errs("       nice            (print the current nice value)\n");
}

int main(int argc, char **argv)
{
    int inc = 10;
    int i = 1;

    if (argc == 1) {                 /* no command: report and stop */
        int n = sys_nice_get(0);
        if (n == SCHED_E_SRCH) { errs("nice: no such process\n"); return 1; }
        outn(n); outc('\n');
        return 0;
    }

    /* -n INC, and the bare "-N" form GNU accepts is deliberately NOT taken:
     * `nice -5 cmd` reads as "-n 5" to some people and "-n -5" to others, and
     * this is the one program where guessing the sign wrong is the difference
     * between demoting a build and trying to promote it. One spelling only. */
    if (i < argc && c_streq(argv[i], "-n")) {
        if (i + 1 >= argc) { usage(); return 2; }
        inc = c_atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) { usage(); return 2; }

    {
        int cur = sys_nice_get(0);
        if (cur == SCHED_E_SRCH) cur = 0;
        int r = sys_nice_set(0, cur + inc);
        if (r == SCHED_E_PERM) {
            errs("nice: permission denied (only root may raise priority)\n");
            return 1;
        }
        if (r == SCHED_E_SRCH || r == SCHED_E_INVAL) {
            errs("nice: cannot set priority\n");
            return 1;
        }
    }

    sys_execve(argv[i], &argv[i], 0);
    /* Only reached if execve failed -- it does not return on success. */
    errs("nice: cannot run ");
    errs(argv[i]);
    errs("\n");
    return 127;
}
