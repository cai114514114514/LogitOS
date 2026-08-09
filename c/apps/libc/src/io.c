#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "logit_abi.h"   /* SYS_* numbers (shared kernel ABI; -Iinclude/abi) */

/* POSIX-ish syscall layer over int 0x80. errno is defined here (the one TU).
 *
 * ERRNO DISCIPLINE. The kernel returns a single -1 for every failure -- it has
 * no errno space of its own -- so this layer maps each call to the errno a C
 * program would expect from THAT call. That is a guess, and it is a much better
 * one than EIO-for-everything, which is what a caller printing strerror(errno)
 * used to get from a missing file. Where a call can fail two ways that a
 * program routinely distinguishes (open: missing vs. no permission), the errno
 * chosen is the one that is true on this system: LogitFS has no permissions, so
 * an open that fails, failed because the path is not there.
 *
 * errno is NEVER cleared on success, as C requires: a caller may set it to 0,
 * make a call that succeeds, and rely on it still being 0. */
int errno;

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

static long fail(long r, int e) { if (r < 0) errno = e; return r; }

ssize_t read(int fd, void *buf, size_t n)
{
    long r = sys(SYS_READ, fd, (long)buf, (long)n);
    /* The kernel signals "non-blocking, nothing yet" with EAGAIN_RC (-2), which
     * is a different thing from an error and from EOF; a caller polling a pipe
     * has to be able to tell all three apart. */
    if (r == EAGAIN_RC) { errno = EAGAIN; return -1; }
    /* EINTR. A blocking read cut short by a delivered signal is NOT an error of
     * the read and must not be reported as one -- a program that retries on
     * EINTR and gives up on EBADF has to be able to tell them apart. The kernel
     * says so with its own value rather than with -1, because -1 already means
     * "bad descriptor" here; see SIG_E_INTR in include/abi/logit_abi.h. */
    if (r == SIG_E_INTR) { errno = EINTR; return -1; }
    if (r < 0) errno = EBADF;
    return r;
}
ssize_t write(int fd, const void *buf, size_t n)
{
    long r = sys(SYS_WRITE, fd, (long)buf, (long)n);
    if (r == EAGAIN_RC) { errno = EAGAIN; return -1; }
    if (r == SIG_E_INTR) { errno = EINTR; return -1; }
    if (r < 0) errno = EBADF;
    return r;
}
int     close(int fd)                            { return (int)fail(sys(SYS_CLOSE, fd, 0, 0), EBADF); }
off_t   lseek(int fd, off_t off, int whence)
{
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { errno = EINVAL; return -1; }
    return fail(sys(SYS_LSEEK, fd, off, whence), ESPIPE);
}
int     fsync(int fd)                            { return (int)fail(sys(SYS_FSYNC, fd, 0, 0), EBADF); }
int     fdatasync(int fd)                        { return fsync(fd); }
int     dup(int fd)                              { return (int)fail(sys(SYS_DUP, fd, 0, 0), EBADF); }
int     dup2(int o, int nw)                      { return (int)fail(sys(SYS_DUP2, o, nw, 0), EBADF); }
int     pipe(int fds[2])                         { return (int)fail(sys(SYS_PIPE, (long)fds, 0, 0), EMFILE); }
int     chdir(const char *p)                     { return (int)fail(sys(SYS_CHDIR, (long)p, 0, 0), ENOENT); }
int     unlink(const char *p)                    { return (int)fail(sys(SYS_DELETE_FILE, (long)p, 0, 0), ENOENT); }
int     mkdir(const char *p, mode_t mode)        { (void)mode; return (int)fail(sys(SYS_MKDIR, (long)p, 0, 0), EEXIST); }
int     rename(const char *a, const char *b)     { return (int)fail(sys(SYS_RENAME, (long)a, (long)b, 0), ENOENT); }
pid_t   getpid(void)                             { return (int)sys(SYS_GETPID, 0, 0, 0); }
pid_t   fork(void)                               { return (int)fail(sys(SYS_FORK, 0, 0, 0), EAGAIN); }
void    _exit(int code)                          { sys(SYS_EXIT, code, 0, 0); for (;;) {} }

/* rmdir must refuse a non-directory, which the shared delete syscall does not
 * check; without this, rmdir("file.txt") silently deleted the file. */
int rmdir(const char *p)
{
    if (sys(SYS_DIR_COUNT, (long)p, 0, 0) < 0) { errno = ENOTDIR; return -1; }
    return (int)fail(sys(SYS_DELETE_FILE, (long)p, 0, 0), ENOTEMPTY);
}

/* isatty is a real query now: fds 0-2 are only a terminal if they have not been
 * redirected, and a seekable fd is a file. A pipe and the tty both fail to
 * seek, so the tty is "unseekable and one of the three standard descriptors" --
 * which is exactly what the shell arranges. */
int isatty(int fd)
{
    if (fd < 0) { errno = EBADF; return 0; }
    if (sys(SYS_LSEEK, fd, 0, SEEK_CUR) >= 0) { errno = ENOTTY; return 0; }
    if (fd <= 2) return 1;
    errno = ENOTTY;
    return 0;
}

int open(const char *path, int flags, ...)
{ return (int)fail(sys(SYS_OPEN, (long)path, flags, 0), ENOENT); }
int creat(const char *path, int mode) { (void)mode; return open(path, O_WRONLY | O_CREAT | O_TRUNC); }

int access(const char *path, int mode)
{
    /* LogitFS has no permission bits, so this answers the only question it can:
     * does the path exist? R_OK/W_OK/X_OK all succeed for anything that does. */
    (void)mode;
    if (sys(SYS_DIR_COUNT, (long)path, 0, 0) >= 0) return 0;   /* a directory */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { errno = ENOENT; return -1; }
    close(fd);
    return 0;
}

int ftruncate(int fd, off_t len)
{
    /* There is no truncate syscall. Growing works (seek past the end and write
     * a zero byte); shrinking does not, and says so rather than reporting a
     * success that leaves the old bytes in place. */
    long cur = sys(SYS_LSEEK, fd, 0, SEEK_CUR);
    long end = sys(SYS_LSEEK, fd, 0, SEEK_END);
    if (cur < 0 || end < 0) { errno = EBADF; return -1; }
    if (len < end) { sys(SYS_LSEEK, fd, cur, SEEK_SET); errno = ENOSYS; return -1; }
    if (len > end) {
        char z = 0;
        sys(SYS_LSEEK, fd, len - 1, SEEK_SET);
        if (sys(SYS_WRITE, fd, (long)&z, 1) != 1) { errno = EIO; return -1; }
    }
    sys(SYS_LSEEK, fd, cur, SEEK_SET);
    return 0;
}
int truncate(const char *path, off_t len)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    int r = ftruncate(fd, len);
    close(fd);
    return r;
}

char *getcwd(char *buf, size_t size)
{
    if (!buf || size == 0) { errno = EINVAL; return 0; }
    long r = sys(SYS_GETCWD, (long)buf, (long)size, 0);
    if (r < 0) { errno = ERANGE; return 0; }
    return buf;
}
int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }   /* no fd-to-path mapping */

pid_t waitpid(pid_t pid, int *status, int opts)
{
    long r = sys(SYS_WAITPID, pid, (long)status, opts);
    /* Interrupted by a signal, not "no such child". A shell that cannot tell
     * those apart either exits on Ctrl+C or loops forever on a reaped child. */
    if (r == SIG_E_INTR) { errno = EINTR; return -1; }
    return (int)fail(r, ECHILD);
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }

/* There is no parent-pid syscall; a process that needs its parent must have
 * been told. Reporting 1 (init) would be a plausible-looking invention. */
pid_t getppid(void) { errno = ENOSYS; return -1; }
/* M32: these used to return a hardcoded 0, with a comment saying "one user, no
 * security model: uid 0 is the truth, not a placeholder". That was true when
 * it was written and is no longer -- SYS_GETUID (150) exists and there is a
 * /bin/login that sets it, so a program asking who it is now gets an answer
 * rather than a constant.
 *
 * geteuid/getegid are ALIASES and will stay aliases until this filesystem has
 * a setuid bit; see the block comment on SYS_GETEUID in logit_abi.h. They are
 * not "unimplemented" -- on this machine they are equal by construction. */
uid_t getuid(void)  { return (uid_t)sys(SYS_GETUID, 0, 0, 0); }
uid_t geteuid(void) { return (uid_t)sys(SYS_GETEUID, 0, 0, 0); }
gid_t getgid(void)  { return (gid_t)sys(SYS_GETGID, 0, 0, 0); }
gid_t getegid(void) { return (gid_t)sys(SYS_GETEGID, 0, 0, 0); }

int setuid(uid_t uid)
{
    long r = sys(SYS_SETUID, (long)uid, 0, 0);
    if (r < 0) { errno = EPERM; return -1; }
    return 0;
}
int setgid(gid_t gid)
{
    long r = sys(SYS_SETGID, (long)gid, 0, 0);
    if (r < 0) { errno = EPERM; return -1; }
    return 0;
}

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

/* Coarse sleep: poll the monotonic clock and yield. Uses the MONOTONIC clock,
 * not the RTC -- the old version polled time(), so setting the wall clock
 * backwards during a sleep() hung the caller until the clock caught up. */
unsigned sleep(unsigned secs)
{
    unsigned long long t0 = (unsigned long long)sys(SYS_MONOTONIC_MS, 0, 0, 0);
    unsigned long long want = (unsigned long long)secs * 1000ull;
    while ((unsigned long long)sys(SYS_MONOTONIC_MS, 0, 0, 0) - t0 < want)
        sys(SYS_YIELD, 0, 0, 0);
    return 0;
}
int usleep(unsigned usecs)
{
    unsigned long long t0 = (unsigned long long)sys(SYS_MONOTONIC_MS, 0, 0, 0);
    unsigned long long want = usecs / 1000u;
    while ((unsigned long long)sys(SYS_MONOTONIC_MS, 0, 0, 0) - t0 < want)
        sys(SYS_YIELD, 0, 0, 0);
    return 0;
}

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:  return 4096;
    case _SC_OPEN_MAX:  return OPEN_MAX;
    case _SC_CLK_TCK:   return 100;          /* the kernel's PIT rate */
    case _SC_ARG_MAX:   return ARG_MAX;
    case _SC_NPROCESSORS_ONLN: return sys(SYS_CPU_INDEX, 0, 0, 0) >= 0 ? 4 : 1;
    default: errno = EINVAL; return -1;
    }
}

int gethostname(char *buf, size_t n)
{
    static const char h[] = "logit";
    if (!buf || n == 0) { errno = EINVAL; return -1; }
    if (n < sizeof h) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, h, sizeof h);
    return 0;
}

/* fcntl: only the commands the kernel can actually perform.
 *
 * F_DUPFD and F_GETFL/F_SETFL's O_NONBLOCK bit are real (SYS_DUP, SYS_SETNB).
 * F_GETFD/F_SETFD accept and discard FD_CLOEXEC because exec here does not
 * close anything -- a program setting it is not harmed, and one READING it back
 * gets what it set. Anything else fails with EINVAL rather than returning 0 and
 * letting the caller believe a lock was taken. */
int fcntl(int fd, int cmd, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    long arg = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);

    static int cloexec[OPEN_MAX];
    switch (cmd) {
    case F_DUPFD: {
        int nf = (int)fail(sys(SYS_DUP, fd, 0, 0), EBADF);
        return nf;                       /* cannot honour the "lowest >= arg" part */
    }
    case F_GETFD: return (fd >= 0 && fd < OPEN_MAX) ? cloexec[fd] : (errno = EBADF, -1);
    case F_SETFD:
        if (fd < 0 || fd >= OPEN_MAX) { errno = EBADF; return -1; }
        cloexec[fd] = (int)(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL: return O_RDWR;         /* the kernel does not record the mode */
    case F_SETFL:
        if (arg & O_NONBLOCK) return (int)fail(sys(SYS_SETNB, fd, 0, 0), EBADF);
        return 0;                        /* clearing O_NONBLOCK is not supported */
    default:
        errno = EINVAL;
        return -1;
    }
}
