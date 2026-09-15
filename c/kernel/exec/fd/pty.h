#ifndef LOGIT_KERNEL_PTY_H
#define LOGIT_KERNEL_PTY_H
#include "file.h"
#include "../../../../include/abi/pty.h"
int pty_open(struct file **master, struct file **slave);
long pty_read(struct file *, void *, long);
long pty_write(struct file *, const void *, long);
short pty_poll(struct file *, struct poll_table *);
void pty_release(void *, int);
long pty_syscall(long, long, long, long);
#endif
