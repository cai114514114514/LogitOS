#include <pty.h>
#include <unistd.h>
#include <errno.h>
#include "../../../../include/abi/pty.h"

static long pty_open_pair(int fds[2])
{
#if __STDC_HOSTED__
    (void)fds;
    return -1;
#else
    long r;
    __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_PTY_OPEN),"D"((long)fds),"S"(0L),"d"(0L):"memory");
    return r;
#endif
}

int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp)
{
    if(!amaster||!aslave){errno=EINVAL;return -1;}
    int fds[2];
    if(pty_open_pair(fds)<0){errno=EMFILE;return -1;}
    if(termp&&tcsetattr(fds[1],TCSANOW,termp)<0)goto fail;
    if(winp&&ioctl(fds[1],TIOCSWINSZ,(void *)winp)<0)goto fail;
    if(name)name[0]=0;
    *amaster=fds[0];*aslave=fds[1];
    return 0;
fail:
    close(fds[0]);close(fds[1]);return -1;
}
