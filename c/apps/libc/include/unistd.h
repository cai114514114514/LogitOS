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

/* Links and ownership. Real as of the file-metadata work (SYS_SYMLINK /
 * SYS_LINK / SYS_READLINK / SYS_CHOWN); implemented in dirstat.c beside stat().
 * WORTH KNOWING: symlink targets and hard-link groups are VFS records kept in
 * RAM -- the link works, and it is gone after a reboot. A file's MODE and OWNER
 * do survive, because those reach the inode. */
int     symlink(const char *target, const char *linkpath);
int     link(const char *oldpath, const char *newpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int     chown(const char *path, uid_t uid, gid_t gid);
int     rmdir(const char *path);
int     chdir(const char *path);
int     fchdir(int fd);
int     truncate(const char *path, off_t len);
int     ftruncate(int fd, off_t len);
char   *getcwd(char *buf, size_t size);
pid_t   getpid(void);
pid_t   getppid(void);
/* Real answers since M32, not the constant 0 they used to be. geteuid ==
 * getuid and getegid == getgid by construction: there is no setuid bit in this
 * filesystem, so nothing can ever make them differ (logit_abi.h says why at
 * length). setuid/setgid are root-only and ONE-WAY -- there is no saved-set
 * to come back through, so a process that has dropped stays dropped. */
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
pid_t   fork(void);
int     execv(const char *path, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execvp(const char *file, char *const argv[]);
void    _exit(int code);
unsigned sleep(unsigned secs);
int     usleep(unsigned usecs);
long    sysconf(int name);
int     gethostname(char *buf, size_t n);

/* nice() is declared here because POSIX puts it here, and IMPLEMENTED in
 * c/apps/libc/src/resource.c next to getpriority/setpriority, which it is
 * defined in terms of -- splitting it into its own TU would put two halves of
 * one rule in two files. It is the 4.3BSD/POSIX form: RELATIVE increment,
 * returns the NEW nice value, and -1 is therefore a legal success. Clear errno
 * before calling, as with getpriority(). See <sys/resource.h>. */
int     nice(int inc);

/* getopt() -- POSIX declares the short-option half here, not in <getopt.h>.
 * <getopt.h> (c/apps/libc/include/getopt.h) includes this header for exactly
 * that reason, and adds only getopt_long()/struct option on top. A ported
 * program that #includes just <unistd.h> and calls getopt() must find it
 * here; see c/apps/libc/src/getopt.c for the implementation and its glibc-vs-
 * musl argv-permutation note. */
int     getopt(int argc, char *const argv[], const char *optstring);
extern char *optarg;
extern int   optind, opterr, optopt;

/* sysconf() names -- only the ones LogitOS can honestly answer. */
#define _SC_PAGESIZE       30
#define _SC_PAGE_SIZE      _SC_PAGESIZE
#define _SC_OPEN_MAX        4
#define _SC_NPROCESSORS_ONLN 84
#define _SC_CLK_TCK         2
#define _SC_ARG_MAX         0

#endif /* _UNISTD_H */
