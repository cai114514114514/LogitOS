/* <termios.h>. Historically there was no termios driver on this kernel, so
 * every stateful call failed with ENOTTY rather than fabricating success.
 * Correction (2026-09-15): native PTYs have the operations routed below; the
 * serial console and unsupported operations retain that honest failure. The
 * pure struct helpers (cfmakeraw and the speed accessors) need no driver. */
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "pty_ctl.h"
_Static_assert(sizeof(struct termios)==sizeof(struct logit_termios),"one terminal attribute ABI");
static int pty_result(long r) { if(r<0){errno=ENOTTY;return -1;}return 0; }

static int not_a_pty(int fd)
{
    if (!isatty(fd)) { errno = ENOTTY; return 1; }
    /* fd IS the console tty, but there is still no attribute store behind
     * it -- see the header. */
    errno = ENOTTY;
    return 1;
}

/* 2026-09-11: PTY descriptors now own real attributes; the historical
 * ENOTTY description above still applies to the legacy serial console. */
int tcgetattr(int fd, struct termios *t) { return pty_result(libc_pty_ctl(fd,LPTY_GETATTR,t)); }
int tcsetattr(int fd, int actions, const struct termios *t)
{
    if(actions<0||actions>2){errno=EINVAL;return -1;}
    if(actions!=TCSANOW && tcdrain(fd)<0)return -1;
    if(actions==TCSAFLUSH && tcflush(fd,TCIFLUSH)<0)return -1;
    return pty_result(libc_pty_ctl(fd,LPTY_SETATTR,(void *)t));
}
int tcflush(int fd, int queue) { return pty_result(libc_pty_ctl(fd,LPTY_FLUSH,(void *)(long)queue)); }
int tcdrain(int fd) { return pty_result(libc_pty_ctl(fd,LPTY_DRAIN,0)); }
int tcflow(int fd, int action)
{
    /* IXON/IXOFF and transmitter suspension are not in the PTY discipline.
     * Before PTYs were reachable not_a_pty() happened to reject every fd; now
     * an isatty-success path must not turn this into a no-op success. */
    (void)action;
    (void)not_a_pty(fd);
    return -1;
}
pid_t tcgetpgrp(int fd)
{
    long r=libc_pty_ctl(fd,LPTY_GETPGRP,0);
    if(r<0){errno=ENOTTY;return -1;}
    return (pid_t)r;
}
int tcsetpgrp(int fd, pid_t pgrp)
{
    if(pgrp<=0){errno=EINVAL;return -1;}
    if(!isatty(fd)){errno=ENOTTY;return -1;}
    if(libc_pty_ctl(fd,LPTY_SETPGRP,(void *)(long)pgrp)<0){errno=EPERM;return -1;}
    return 0;
}

speed_t cfgetispeed(const struct termios *t) { return t ? t->c_ispeed : 0; }
speed_t cfgetospeed(const struct termios *t) { return t ? t->c_ospeed : 0; }
int cfsetispeed(struct termios *t, speed_t speed) { if (!t) { errno = EINVAL; return -1; } t->c_ispeed = speed; return 0; }
int cfsetospeed(struct termios *t, speed_t speed) { if (!t) { errno = EINVAL; return -1; } t->c_ospeed = speed; return 0; }

void cfmakeraw(struct termios *t)
{
    if (!t) return;
    t->c_iflag &= ~(tcflag_t)(ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}
