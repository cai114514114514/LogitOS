/* POSIX descriptor adapter only; the SFTP parser/dispatcher remains the
 * product TU. Used by stock OpenSSH's -D local-subsystem integration mode. */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
enum { HOST_RDONLY=O_RDONLY, HOST_WRONLY=O_WRONLY, HOST_RDWR=O_RDWR,
       HOST_CREAT=O_CREAT, HOST_TRUNC=O_TRUNC, HOST_APPEND=O_APPEND };
#undef O_RDONLY
#undef O_WRONLY
#undef O_RDWR
#undef O_CREAT
#undef O_TRUNC
#undef O_APPEND
#undef O_NONBLOCK
#include "logit_abi.h"
static int cv(int n) { return n < 0 ? -errno : n; }
static int sys_read(int fd, void *b, int n) { return cv(read(fd,b,n)); }
static int sys_write(int fd, const void *b, int n) { return cv(write(fd,b,n)); }
static int sys_open(const char *p, int f)
{ int m=(f&3)==O_RDWR?HOST_RDWR:(f&3)==O_WRONLY?HOST_WRONLY:HOST_RDONLY;
  if(f&O_CREAT)m|=HOST_CREAT; if(f&O_TRUNC)m|=HOST_TRUNC; if(f&O_APPEND)m|=HOST_APPEND;
  return cv(open(p,m,0666)); }
static int sys_close(int fd) { return cv(close(fd)); }
static long sys_lseek(int fd,long o,int w) { return lseek(fd,o,w); }
static int sys_getcwd(char *p,int n) { return getcwd(p,n)?0:-1; }
static int make_dir(const char *p) { return cv(mkdir(p,0777)); }
static int delete_file(const char *p) { return cv(remove(p)); }
static int sys_rename(const char *p,const char *q) { return cv(rename(p,q)); }
static int st_chmod(const char *p,int m) { return cv(chmod(p,m)); }
static int st_chown(const char *p,int u,int g) { return cv(chown(p,u,g)); }
static int st_readlink(const char *p,char *b,int n) { return cv(readlink(p,b,n)); }
static int st_symlink(const char *target,const char *p) { return cv(symlink(target,p)); }
static int st_link(const char *old,const char *p) { return cv(link(old,p)); }
static void cvstat(struct logit_stat *d,const struct stat *s)
{ memset(d,0,sizeof *d);d->mode=s->st_mode;d->size=s->st_size;d->uid=s->st_uid;d->gid=s->st_gid;d->ino=s->st_ino;d->dev=s->st_dev;d->attr=LSTA_TIMES;d->atime=s->st_atime;d->mtime=s->st_mtime; }
static int st_stat(const char *p,struct logit_stat *d) { struct stat s;if(stat(p,&s)<0)return -1;cvstat(d,&s);return 0; }
static int st_lstat(const char *p,struct logit_stat *d) { struct stat s;if(lstat(p,&s)<0)return -1;cvstat(d,&s);return 0; }
static int st_fstat(int fd,struct logit_stat *d) { struct stat s;if(fstat(fd,&s)<0)return -1;cvstat(d,&s);return 0; }
static int st_getdents(const char *p,int *cursor,struct logit_dirent *b,int max)
{ DIR *d=opendir(p);if(!d)return -1;struct dirent *e;int i=0,n=0;
  while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
    if(i++<*cursor)continue;memset(&b[n],0,sizeof b[n]);snprintf(b[n++].name,256,"%s",e->d_name);if(n==max)break;}
  closedir(d);*cursor+=n;return n; }
static long _sys(int op,long a,long b,long c)
{ (void)c;if(op==SYS_FTRUNCATE)return cv(ftruncate(a,b));if(op==SYS_FSYNC)return cv(fsync(a));return -1; }
static int c_strlen(const char *s) { return strlen(s); }
static int c_streq(const char *a,const char *b) { return !strcmp(a,b); }
static void c_strcpy(char *d,const char *s,int n) { snprintf(d,n,"%s",s); }
