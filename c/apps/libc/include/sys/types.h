#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>

#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef long ssize_t;
#endif
#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif

typedef long   off64_t;
typedef int    pid_t;
typedef int    uid_t;
typedef int    gid_t;
typedef int    mode_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef long   blksize_t;
typedef long   blkcnt_t;
typedef long   suseconds_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

#endif
