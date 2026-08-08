#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <sys/types.h>

/* There is no termios driver on LogitOS -- no raw/cooked mode switch, no
 * baud rate, no line discipline. The Terminal app (c/apps/gui/terminal) and
 * the serial console do their OWN line editing/echo in the kernel or in the
 * app that owns the tty; nothing exposes that state through an ioctl a ring-3
 * program can read or change (c/kernel/exec/file.c's F_TTY is a plain byte
 * stream). So every call here is a HONEST FAILURE (ENOTTY) rather than a
 * struct of invented flags a program would believe it successfully set.
 * A program that checks the return value before relying on raw mode --
 * which a correct one must, since tcgetattr can fail on any real system too
 * (redirected stdin, for one) -- behaves correctly here: it sees the failure
 * and falls back to whatever it does for "not a controllable terminal." */

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed, c_ospeed;
};

/* Just enough symbolic constants that code referencing them compiles; none
 * of them can actually be set (see above). */
#define ICANON 0x0002
#define ECHO   0x0008
#define ECHOE  0x0010
#define ISIG   0x0001
#define IXON   0x0400
#define ICRNL  0x0100
#define OPOST  0x0001
#define VMIN   6
#define VTIME  5
#define VINTR  0
#define VEOF   4
#define VERASE 2

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);
int tcflush(int fd, int queue);
int tcdrain(int fd);
int tcflow(int fd, int action);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t speed);
int cfsetospeed(struct termios *t, speed_t speed);
void cfmakeraw(struct termios *t);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);

#endif /* _TERMIOS_H */
