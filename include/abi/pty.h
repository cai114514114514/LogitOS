#ifndef LOGIT_PTY_ABI_H
#define LOGIT_PTY_ABI_H
/* Native PTY: a master and a slave are ordinary inherited/dup-able fds.
 * The master (sshd/terminal) consumes signal events and delivers them to its
 * owned child. This OS has no POSIX sessions/process groups; claiming those
 * from a stored pid would introduce a pid-reuse race after a child exits. */
#define SYS_PTY_OPEN 194
#define SYS_PTY_CTL 195
#define LPTY_GETATTR 1
#define LPTY_SETATTR 2
#define LPTY_GETWIN 3
#define LPTY_SETWIN 4
#define LPTY_SIGNAL 5 /* master only: pop a signal number, 0 if none */
#define LPTY_FLUSH 6 /* argument 0 input, 1 output, 2 both */
#define LPTY_DRAIN 7 /* wait until the master has read all slave output */
struct logit_termios {
    unsigned int iflag, oflag, cflag, lflag;
    unsigned char cc[32];
    unsigned int ispeed, ospeed;
};
struct logit_winsize { unsigned short rows, cols, xpixel, ypixel; };
#define LPTY_ISIG 1u
#define LPTY_ICANON 2u
#define LPTY_ECHO 8u
#define LPTY_ECHOE 16u
#define LPTY_ICRNL 256u
#define LPTY_OPOST 1u
#define LPTY_ONLCR 4u
#define LPTY_VINTR 0
#define LPTY_VQUIT 1
#define LPTY_VERASE 2
#define LPTY_VKILL 3
#define LPTY_VEOF 4
#define LPTY_VTIME 5
#define LPTY_VMIN 6
#endif
