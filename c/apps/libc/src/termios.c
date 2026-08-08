/* <termios.h>. See the header: there is no termios driver on this kernel, so
 * every call that would change or report real terminal state fails with
 * ENOTTY -- honestly, not with a fabricated struct. The few calls that need
 * no kernel state (cfmakeraw, the speed accessors) work on the caller's own
 * struct, exactly as they do on any system, because they are pure struct
 * manipulation the standard defines that way. */
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static int not_a_pty(int fd)
{
    if (!isatty(fd)) { errno = ENOTTY; return 1; }
    /* fd IS the console tty, but there is still no attribute store behind
     * it -- see the header. */
    errno = ENOTTY;
    return 1;
}

int tcgetattr(int fd, struct termios *t) { (void)t; return not_a_pty(fd) ? -1 : 0; }
int tcsetattr(int fd, int actions, const struct termios *t) { (void)actions; (void)t; return not_a_pty(fd) ? -1 : 0; }
int tcflush(int fd, int queue) { (void)queue; return not_a_pty(fd) ? -1 : 0; }
int tcdrain(int fd) { return not_a_pty(fd) ? -1 : 0; }
int tcflow(int fd, int action) { (void)action; return not_a_pty(fd) ? -1 : 0; }
pid_t tcgetpgrp(int fd) { not_a_pty(fd); return -1; }
int tcsetpgrp(int fd, pid_t pgrp) { (void)pgrp; return not_a_pty(fd) ? -1 : 0; }

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
