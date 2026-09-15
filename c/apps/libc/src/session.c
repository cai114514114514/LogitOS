#include <unistd.h>
#include <errno.h>
#include "../../../../include/abi/pty.h"

static long session_call(long n,long a,long b)
{
#if __STDC_HOSTED__
    (void)n;(void)a;(void)b;return -1;
#else
    long r;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(0L):"memory");
    return r;
#endif
}

pid_t setsid(void)
{
    long r=session_call(SYS_SETSID,0,0);
    if(r<0){errno=EPERM;return -1;}return (pid_t)r;
}
int setpgid(pid_t pid,pid_t pgid)
{
    if(pid<0||pgid<0){errno=EINVAL;return -1;}
    long r=session_call(SYS_SETPGID,pid,pgid);
    if(r<0){errno=EPERM;return -1;}return 0;
}
pid_t getpgid(pid_t pid)
{
    if(pid<0){errno=EINVAL;return -1;}
    long r=session_call(SYS_GETPGID,pid,0);
    if(r<0){errno=ESRCH;return -1;}return (pid_t)r;
}
pid_t getpgrp(void){return getpgid(0);}
