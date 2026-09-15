#ifndef _PTY_H
#define _PTY_H

#include <termios.h>
#include <sys/ioctl.h>

/* LogitOS PTYs are anonymous descriptor pairs: there is deliberately no slave
 * pathname because /dev is synthetic and cannot host nodes created at run
 * time. `name`, when supplied, receives an empty string rather than a invented
 * /dev/pts path. forkpty/login_tty remain absent: callers can spell the small
 * sequence explicitly with fork, setsid, TIOCSCTTY and dup2, which keeps the
 * process-lifecycle policy out of the allocation primitive. */
int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp);

#endif /* _PTY_H */
