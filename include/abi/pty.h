#ifndef LOGIT_PTY_ABI_H
#define LOGIT_PTY_ABI_H
/* Native PTY: a master and a slave are ordinary inherited/dup-able fds.
 * There is deliberately no /dev/ptmx name: /dev is a fixed synthetic
 * namespace and this machine has no mknod, so routing the already-atomic pair
 * allocation through pathname lookup would add a third synthetic-file
 * mechanism without making the terminal any more Unix-like. openpty() is the
 * ring-3 spelling of SYS_PTY_OPEN.
 *
 * The master (sshd/terminal) consumes signal events and delivers them to its
 * owned child. The historical contract said this OS had no POSIX sessions or
 * process groups because storing one foreground pid would race pid reuse.
 * Correction (2026-09-15): the process table now owns sid/pgid membership and
 * a PTY stores the controlling session plus a validated foreground pgid.
 * Background-read stops, orphan-group rules, and tty-generated group signal
 * delivery remain absent rather than being approximated. */
#define SYS_PTY_OPEN 194
#define SYS_PTY_CTL 195
/* 196 is SYS_CPU_COUNT and 197 is SYS_FSREF in their existing ABI headers. */
#define SYS_SETSID   198
#define SYS_SETPGID  199
#define SYS_GETPGID  200
#define LPTY_GETATTR 1
#define LPTY_SETATTR 2
#define LPTY_GETWIN 3
#define LPTY_SETWIN 4
#define LPTY_SIGNAL 5 /* master only: pop a signal number, 0 if none */
#define LPTY_FLUSH 6 /* argument 0 input, 1 output, 2 both */
#define LPTY_DRAIN 7 /* wait until the master has read all slave output */
#define LPTY_SETCTTY 8 /* slave only: attach caller's new session */
#define LPTY_GETPGRP 9 /* controlling session only: -> foreground pgid */
#define LPTY_SETPGRP 10 /* controlling session only: argument is a live pgid */
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
