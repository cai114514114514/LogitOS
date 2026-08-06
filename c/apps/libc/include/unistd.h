#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef long off_t;
typedef int  pid_t;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     fsync(int fd);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     isatty(int fd);
int     unlink(const char *path);
int     rmdir(const char *path);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
pid_t   getpid(void);
pid_t   fork(void);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
void    _exit(int code);
unsigned sleep(unsigned secs);

#endif /* _UNISTD_H */
