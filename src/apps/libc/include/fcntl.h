#ifndef _FCNTL_H
#define _FCNTL_H

/* open() flags -- must match the kernel ABI (include/abi/aether_abi.h). */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800

int open(const char *path, int flags, ...);
int creat(const char *path, int mode);

#endif /* _FCNTL_H */
