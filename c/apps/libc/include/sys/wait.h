#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>

/* The kernel's wait status (c/kernel/exec/proc.c) is the same encoding Linux
 * uses: low byte = terminating signal, next byte = exit code. LogitOS never
 * terminates a process by signal, so WIFSIGNALED is always false and
 * WIFEXITED always true for a reaped child -- the macros are still written
 * out in full so that code branching on them behaves. */
#define WNOHANG   1
#define WUNTRACED 2

#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WSTOPSIG(s)    WEXITSTATUS(s)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WCOREDUMP(s)   ((s) & 0x80)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* _SYS_WAIT_H */
