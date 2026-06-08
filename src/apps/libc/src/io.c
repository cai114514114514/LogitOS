#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "aether_abi.h"   /* SYS_* numbers (shared kernel ABI; -Iinclude/abi) */

/* POSIX-ish syscall layer over int 0x80. errno is defined here (the one TU). */
int errno;

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

static long fail(long r, int e) { if (r < 0) errno = e; return r; }

ssize_t read(int fd, void *buf, size_t n)        { return fail(sys(SYS_READ, fd, (long)buf, (long)n), EIO); }
ssize_t write(int fd, const void *buf, size_t n) { return fail(sys(SYS_WRITE, fd, (long)buf, (long)n), EIO); }
int     close(int fd)                            { return (int)fail(sys(SYS_CLOSE, fd, 0, 0), EBADF); }
off_t   lseek(int fd, off_t off, int whence)     { return fail(sys(SYS_LSEEK, fd, off, whence), EINVAL); }
int     dup(int fd)                              { return (int)fail(sys(SYS_DUP, fd, 0, 0), EBADF); }
int     dup2(int o, int nw)                      { return (int)fail(sys(SYS_DUP2, o, nw, 0), EBADF); }
int     pipe(int fds[2])                         { return (int)fail(sys(SYS_PIPE, (long)fds, 0, 0), EMFILE); }
int     chdir(const char *p)                     { return (int)fail(sys(SYS_CHDIR, (long)p, 0, 0), ENOENT); }
int     unlink(const char *p)                    { return (int)fail(sys(SYS_DELETE_FILE, (long)p, 0, 0), ENOENT); }
int     rmdir(const char *p)                     { return (int)fail(sys(SYS_DELETE_FILE, (long)p, 0, 0), ENOENT); }
int     mkdir(const char *p, int mode)           { (void)mode; return (int)fail(sys(SYS_MKDIR, (long)p, 0, 0), EACCES); }
pid_t   getpid(void)                             { return (int)sys(SYS_GETPID, 0, 0, 0); }
pid_t   fork(void)                               { return (int)sys(SYS_FORK, 0, 0, 0); }
void    _exit(int code)                          { sys(SYS_EXIT, code, 0, 0); for (;;) {} }
int     isatty(int fd)                           { return fd >= 0 && fd <= 2; }   /* heuristic: std streams */

int open(const char *path, int flags, ...)
{ return (int)fail(sys(SYS_OPEN, (long)path, flags, 0), ENOENT); }
int creat(const char *path, int mode) { (void)mode; return open(path, O_WRONLY | O_CREAT | O_TRUNC); }

char *getcwd(char *buf, size_t size)
{ long r = sys(SYS_GETCWD, (long)buf, (long)size, 0); if (r < 0) { errno = ERANGE; return 0; } return buf; }

pid_t waitpid(pid_t pid, int *status, int opts)
{ return (int)sys(SYS_WAITPID, pid, (long)status, opts); }

int execve(const char *path, char *const argv[], char *const envp[])
{ return (int)fail(sys(SYS_EXECVE, (long)path, (long)argv, (long)envp), ENOENT); }
int execv(const char *path, char *const argv[]) { return execve(path, argv, 0); }

int execvp(const char *file, char *const argv[])
{
    if (file) for (const char *p = file; *p; p++) if (*p == '/') return execv(file, argv);
    char buf[160] = "/bin/"; int n = 5;
    for (int i = 0; file && file[i] && n < 158; i++) buf[n++] = file[i];
    buf[n] = 0;
    execv(buf, argv);
    return execv(file, argv);   /* fall back to the literal name */
}

/* Coarse sleep: poll the wall clock (RTC) and yield. */
extern long time(long *);   /* time.c */
unsigned sleep(unsigned secs)
{
    long t0 = time(0);
    while ((unsigned long)(time(0) - t0) < secs) sys(SYS_YIELD, 0, 0, 0);
    return 0;
}
