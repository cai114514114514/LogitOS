/* popen() / pclose() -- a FILE piped to/from a forked "/bin/sh -c <cmd>".
 *
 * This is the same fork()+execv("/bin/sh",...) model system() already uses
 * (stdlib.c, around its own comment block) -- read that one first if this
 * looks unfamiliar. The difference is a pipe instead of nothing: popen("r")
 * gives the caller a FILE reading the child's stdout, popen("w") gives one
 * writing the child's stdin.
 *
 * THE FD-LEAK TRAP THIS FILE EXISTS TO AVOID. This kernel's execve() closes
 * NOTHING on its own -- see io.c's fcntl(F_SETFD, FD_CLOEXEC), which only
 * REMEMBERS the bit for F_GETFD to read back; nothing ever acts on it,
 * because there is no close-on-exec here at all. Real popen() protects
 * itself from exactly this by marking its kept pipe end FD_CLOEXEC, so ANY
 * later exec in that process -- not just another popen() -- closes it
 * automatically. Without that primitive, a process that calls popen() twice
 * without pclose()-ing the first forks a SECOND child that inherits the
 * FIRST popen()'s still-open pipe fd. On a machine where a process gets only
 * NFD == 16 descriptors total (c/kernel/exec/proc.h), that is not a
 * theoretical leak -- three or four un-pclose()d popen() calls and a program
 * starts failing pipe()/open() with EMFILE for reasons that have nothing to
 * do with what it just tried to do.
 *
 * The fix has to be done by hand: the child of a NEW popen() closes every
 * OTHER live popen() stream's fd itself, right after fork() and before
 * dup2()/exec(). This rides stdio.c's existing stream registry (the same
 * list fflush(NULL) walks) rather than keeping a second, parallel list here
 * that could drift out of sync with it -- see __libc_close_popen_streams()
 * in stdio.c, and the F_POPEN tag it looks for.
 *
 * WHAT THIS DOES NOT COVER, STATED HONESTLY RATHER THAN LEFT TO BE
 * DISCOVERED: an fd from a plain fopen() (never tagged F_POPEN) still leaks
 * into a popen() child exactly as it would leak into any other exec on this
 * system. That is a pre-existing, system-wide gap -- real close-on-exec
 * would need execve() itself to honour FD_CLOEXEC, which is a kernel change,
 * not something a userland popen() can paper over. A program that fopen()s a
 * log file and then popen()s a long-running child will find that child
 * holding the log fd open. Only the popen-to-popen case is fixed here,
 * because it is the one this library's own popen() creates and can also
 * clean up.
 *
 * pclose() returns the raw wait status (matching glibc/POSIX), NOT an exit
 * code -- exactly like system()'s return value, a caller runs
 * WEXITSTATUS()/WIFEXITED() (sys/wait.h) on it themselves. */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include "libc_internal.h"

FILE *popen(const char *cmd, const char *type)
{
    if (!cmd || !type || (type[0] != 'r' && type[0] != 'w')) { errno = EINVAL; return NULL; }
    int read_side = (type[0] == 'r');   /* "r": we read the child's stdout.
                                          * "w": we write the child's stdin. */

    int fds[2];
    if (pipe(fds) < 0) return NULL;             /* errno set by pipe() */
    int parent_end = read_side ? fds[0] : fds[1];
    int child_end  = read_side ? fds[1] : fds[0];

    long pid = fork();
    if (pid < 0) { int e = errno; close(fds[0]); close(fds[1]); errno = e; return NULL; }

    if (pid == 0) {
        /* --- child --- */
        __libc_close_popen_streams();          /* close every OTHER popen()'s
                                                 * kept fd before this one execs
                                                 * -- see the file comment */
        int target = read_side ? 1 : 0;
        if (child_end != target) { dup2(child_end, target); close(child_end); }
        close(parent_end);                     /* the child never touches this end */

        char *argv[4] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
        execv("/bin/sh", argv);
        _exit(127);                            /* exec failed -- conventional shell "not found" code */
    }

    /* --- parent --- */
    close(child_end);
    FILE *f = __libc_popen_wrap(parent_end, !read_side, pid);
    if (!f) { int e = errno; close(parent_end); errno = e; return NULL; }
    return f;
}

int pclose(FILE *f)
{
    if (!f) { errno = EINVAL; return -1; }
    long pid = __libc_popen_pid(f);
    if (pid < 0) { errno = ECHILD; return -1; }  /* not a stream popen() produced */
    fclose(f);                                    /* closes the pipe fd; the child
                                                    * sees EOF (or, if it is still
                                                    * writing, a failed write) and
                                                    * is expected to exit on its own */
    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) < 0) return -1;
    return status;
}
