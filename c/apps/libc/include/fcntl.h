#ifndef _FCNTL_H
#define _FCNTL_H

/* open() flags -- must match the kernel ABI (include/abi/logit_abi.h). */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800

#define O_EXCL     0x1000     /* accepted and IGNORED: LogitFS open() is not atomic */
#define O_CLOEXEC  0x2000     /* accepted and IGNORED: exec does not close fds here */
#define O_DIRECTORY 0x4000
#define O_NOFOLLOW 0x8000     /* no symlinks exist, so this is always satisfied */
#define AT_FDCWD   (-100)

/* fcntl() commands. Only the ones LogitOS can honour are listed; see fcntl.c. */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

int open(const char *path, int flags, ...);
int creat(const char *path, int mode);
int fcntl(int fd, int cmd, ...);

#endif /* _FCNTL_H */
