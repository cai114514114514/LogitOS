#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef long ssize_t;
#endif
#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif
typedef int  pid_t;
typedef int  uid_t;
typedef int  gid_t;
typedef int  mode_t;

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* access() modes */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     fsync(int fd);
int     fdatasync(int fd);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     isatty(int fd);
int     access(const char *path, int mode);
int     unlink(const char *path);
int     rmdir(const char *path);
int     chdir(const char *path);
int     fchdir(int fd);
int     truncate(const char *path, off_t len);
int     ftruncate(int fd, off_t len);
char   *getcwd(char *buf, size_t size);
pid_t   getpid(void);
pid_t   getppid(void);
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
pid_t   fork(void);
int     execv(const char *path, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execvp(const char *file, char *const argv[]);
void    _exit(int code);
unsigned sleep(unsigned secs);
int     usleep(unsigned usecs);
long    sysconf(int name);
int     gethostname(char *buf, size_t n);

/* sysconf() names -- only the ones LogitOS can honestly answer. */
#define _SC_PAGESIZE       30
#define _SC_PAGE_SIZE      _SC_PAGESIZE
#define _SC_OPEN_MAX        4
#define _SC_NPROCESSORS_ONLN 84
#define _SC_CLK_TCK         2
#define _SC_ARG_MAX         0

#endif /* _UNISTD_H */
