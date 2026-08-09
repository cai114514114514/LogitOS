#ifndef _PWD_H
#define _PWD_H
#include <sys/types.h>

/* THIS USED TO SAY "one user, and it is always root", and until M32 that was
 * true: getuid() returned 0 unconditionally because there was no way to be
 * anything else. There is now -- /etc/passwd, /bin/login and SYS_SETUID -- so
 * these read the real store.
 *
 * TWO THINGS TO KNOW BEFORE BELIEVING A NULL:
 *
 *  - The store is 0600 root:root, so an UNPRIVILEGED process cannot read it
 *    and getpwnam() returns NULL for every name, INCLUDING ITS OWN. There is
 *    no /etc/shadow to split the readable half out into, and
 *    c/apps/coreutils/accounts.h explains why not. A program that wants to
 *    know who it is should call getuid(), which always works.
 *  - "root" always resolves, store or no store, because uid 0 is the identity
 *    the machine boots as rather than an account somebody enrolled.
 *
 * NULL comes with errno left alone, as POSIX requires when a name is simply
 * not found -- so a caller cannot distinguish "no such user" from "you may not
 * look", which on this machine is the honest amount of information to give an
 * unprivileged caller. pw_passwd is always "x" and never the hash. */

struct passwd {
    char  *pw_name;
    char  *pw_passwd;
    uid_t  pw_uid;
    gid_t  pw_gid;
    char  *pw_gecos;
    char  *pw_dir;
    char  *pw_shell;
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

#endif /* _PWD_H */
