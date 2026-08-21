#include "clib.h"

/* renice NICE PID [PID...] -- set the nice value of already-running processes.
 * renice -g PID [PID...]   -- print their nice values and weights instead.
 *
 * The difference from /bin/nice is not cosmetic: nice sets a value on ITSELF
 * and then becomes the program, so it never needs to name another process.
 * This one does, which is why the kernel's SET_NICE takes a pid at all and why
 * the permission rule in c/kernel/sched/sched.c has a "not yours" case as well
 * as a "may not raise" case.
 *
 * ABSOLUTE, NOT RELATIVE, which is the trap POSIX renice contains and this
 * program cannot fix: `nice -n 10` ADDS ten, `renice 10` SETS ten. Running
 * `renice 10 $pid` twice leaves it at 10; running `nice -n 10` twice would
 * reach 20. Both spellings are the standard ones and changing either would
 * make a script written elsewhere do something else here.
 *
 * The weight column exists because the nice value alone does not say what the
 * scheduler will do with it -- 4096 vs 274 is the number the pick loop
 * compares, and printing it is the difference between "I set nice 10" and "the
 * scheduler now gives this process half a share". It is read from the kernel
 * (SCHEDCTL_GET_WEIGHT) rather than recomputed here on purpose: a tool that
 * re-derives the table cannot show the table being wrong.
 */

static void usage(void)
{
    errs("usage: renice NICE PID [PID...]\n");
    errs("       renice -g PID [PID...]   (show nice and weight)\n");
}

static int show(int pid)
{
    int n = sys_nice_get(pid);
    if (n == SCHED_E_SRCH) {
        errs("renice: no such process ");
        outn_fd(2, pid);
        errs("\n");
        return 1;
    }
    outn(pid); outs(": nice="); outn(n);
    outs(" weight="); outn(sys_sched_weight(pid)); outc('\n');
    return 0;
}

int main(int argc, char **argv)
{
    int bad = 0;

    if (argc >= 2 && c_streq(argv[1], "-g")) {
        if (argc < 3) { usage(); return 2; }
        for (int i = 2; i < argc; i++) bad |= show(c_atoi(argv[i]));
        return bad;
    }

    if (argc < 3) { usage(); return 2; }

    int nice = c_atoi(argv[1]);
    for (int i = 2; i < argc; i++) {
        int pid = c_atoi(argv[i]);
        int r = sys_nice_set(pid, nice);
        if (r == SCHED_E_SRCH) {
            errs("renice: no such process "); outn_fd(2, pid); errs("\n");
            bad = 1; continue;
        }
        if (r == SCHED_E_PERM) {
            errs("renice: permission denied for "); outn_fd(2, pid); errs("\n");
            bad = 1; continue;
        }
        if (r == SCHED_E_INVAL) { errs("renice: bad request\n"); bad = 1; continue; }
        /* r is the CLAMPED value the kernel actually installed, which is not
         * necessarily what was asked for -- print it rather than the request,
         * or `renice 100` reports a success that did not happen. */
        outn(pid); outs(": nice="); outn(r);
        outs(" weight="); outn(sys_sched_weight(pid)); outc('\n');
    }
    return bad;
}
