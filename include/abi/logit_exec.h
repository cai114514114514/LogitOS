#ifndef LOGIT_EXEC_ABI_H
#define LOGIT_EXEC_ABI_H

/* The argument-vector contract of SYS_EXECVE / SYS_CAP_SPAWN, shared by the
 * two programs that have to agree on it: c/kernel/exec/exec.c, which copies
 * the vectors into a fixed kernel buffer, and c/apps/coreutils/sh.c, which
 * builds them. It is a separate header rather than two #defines because the
 * two WERE two #defines -- sh.c said 32, exec.c said 48 -- and each end then
 * silently truncated at its own number: sh dropped the 33rd word of a line,
 * exec dropped every argument from the 49th on, neither printed anything, and
 * the truncated command ran (measured on device 2026-08-20). A shell that
 * accepts a command the kernel then shortens is that bug again one layer
 * down, so the only arrangement that cannot drift is one number.
 *
 * WHAT THE NUMBERS ARE SIZED FOR. The first thing on this machine long enough
 * to hit the old limits is a compiler: cc1's own argv for a real file is 32
 * tokens / 436 bytes, and a gcc link line with 30 objects and a dozen -L/-l
 * is 60+ tokens and 2 KiB. 256 entries covers an `ar rcs lib.a *.o` over
 * every file this filesystem can hold (tools/mkfs.py: 256 inodes), and
 * 16 KiB of strings is eight times that link line with the environment
 * (sh's 24 x 160 bytes) taken out first.
 *
 * WHAT THEY COST -- measured, see the commit that raised them: the strings
 * buffer is a kernel-side static (two of them: execve's and cap_spawn's), and
 * the pointer vectors are another 8 bytes an entry, three times over. The
 * initial user stack grows by the bytes actually USED, not by these maxima:
 * exec.c maps the eager head of the stack from the real argv size, so an
 * ordinary `ls` still pays two pages and only a 16 KiB command line pays six.
 *
 * WHAT HAPPENS PAST THEM: LOGIT_EXEC_E2BIG, never a shorter command. */

#define LOGIT_ARG_MAX    256     /* entries in argv, and separately in envp, NOT
                                  * counting the terminating NULL */
#define LOGIT_ARG_BYTES  16384   /* bytes of argv + envp strings TOGETHER, each
                                  * string's NUL included */

/* -E2BIG (errno 7): more than LOGIT_ARG_MAX entries in argv or envp, or their
 * strings do not fit LOGIT_ARG_BYTES. The same spelling SIG_E_INTR uses for
 * -EINTR in logit_abi.h: "this ABI has no errno", so a call that needs a
 * distinguishable failure returns the negated POSIX number by name and a
 * reader who knows the errno table recognises it on sight. -1 stays what it
 * was -- a vector or string the kernel could not read at all. */
#define LOGIT_EXEC_E2BIG (-7)

#endif /* LOGIT_EXEC_ABI_H */
