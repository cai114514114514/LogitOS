#ifndef LOGIT_LIBC_PTY_CTL_H
#define LOGIT_LIBC_PTY_CTL_H
#include "../../../../include/abi/pty.h"
static long libc_pty_ctl(int fd,int command,void *arg)
{
#if __STDC_HOSTED__
    /* Host libc differential tests do not run a LogitOS kernel. Native PTY
     * behavior is verified by the product guest gate. */
    (void)fd;(void)command;(void)arg;return -1;
#else
    long r;__asm__ volatile("int $0x80":"=a"(r):"a"(SYS_PTY_CTL),"D"((long)fd),"S"((long)command),"d"((long)arg):"memory");return r;
#endif
}
#endif
