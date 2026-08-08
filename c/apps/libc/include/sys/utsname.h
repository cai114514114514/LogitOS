#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

/* uname() on LogitOS.
 *
 * Every field below is either a real constant (this IS the kernel this program
 * is running on, always the same one) or the machine's real hostname
 * (gethostname(), <unistd.h>). There is no field here that varies by build --
 * LogitOS does not version itself per commit -- so `release`/`version` name the
 * milestone line from CLAUDE.md's roadmap, which is the closest honest analogue
 * a program asking "what am I running on" can be given. `machine` is real: this
 * is always x86_64 (the only target this tree builds). */

#define UTSNAME_LEN 65   /* matches Linux's _UTSNAME_LENGTH, so a caller sized
                           * for glibc does not truncate here either */

struct utsname {
    char sysname[UTSNAME_LEN];   /* "LogitOS" */
    char nodename[UTSNAME_LEN];  /* gethostname() */
    char release[UTSNAME_LEN];   /* kernel milestone line, e.g. "0.29" (M29) */
    char version[UTSNAME_LEN];   /* build description */
    char machine[UTSNAME_LEN];   /* "x86_64" */
};

int uname(struct utsname *buf);

#endif /* _SYS_UTSNAME_H */
