#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

/* ioctl(): nothing in this tree declared it before this file (checked with
 * `grep -rn ioctl c/kernel c/drivers c/net c/fs c/lib include` -- the only
 * hits were termios.h's prose and string.c's ENOTTY message text), so there
 * is no existing winsize/request-code definition to collide with or match.
 *
 * There is no ioctl SYSCALL either -- no SYS_IOCTL in include/abi/logit_abi.h
 * -- so every request this function knows about is answered out of OTHER
 * real syscalls (fstat/lseek) or refused outright. It never invents a number.
 *
 * The request codes below are the real Linux values (asm-generic/ioctls.h),
 * not ones made up for this file: a ported program that hardcodes
 * TIOCGWINSZ==0x5413 instead of using the macro still works.
 *
 * TIOCGWINSZ / TIOCGPGRP: c/apps/libc/src/termios.c already established the
 * policy for "terminal state ring 3 cannot see" -- ENOTTY, always, because
 * there is no tty driver in this kernel that tracks a window size or a
 * foreground process group (CLAUDE.md: "There are no process groups and no
 * sessions"). ioctl.c's handling of these two is deliberately consistent
 * with that file rather than re-deriving its own answer.
 *
 * FIONREAD: unlike the two above, this one IS answerable, but only for a
 * SEEKABLE fd (an F_VFS regular file) -- bytes remaining to EOF is exactly
 * st_size minus the current offset, and both of those are real numbers off
 * real syscalls (SYS_FSTAT, SYS_LSEEK), the same trick Linux itself uses for
 * FIONREAD on a regular file. A pipe or the tty has no such trick: Linux's
 * FIONREAD on those reports bytes literally queued in a kernel ring buffer,
 * and nothing here exposes that count to ring 3 (see <poll.h>'s comment,
 * which documents the same missing primitive) -- so those fds get ENOSYS,
 * not a guess. */

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540F
#define FIONREAD   0x541B

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
