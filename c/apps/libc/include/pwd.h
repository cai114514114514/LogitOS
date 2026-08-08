#ifndef _PWD_H
#define _PWD_H
#include <sys/types.h>

/* ONE USER, AND IT IS ALWAYS "root". <unistd.h> already says so (getuid()
 * returns 0 unconditionally, because LogitOS has no security model and no
 * second account) -- this header is that same fact in the shape autoconf and
 * every "who am I" call expects. getpwnam() therefore succeeds for "root" and
 * fails (NULL, no errno set -- POSIX leaves it unset when the name is simply
 * not found) for anything else; a program checking "does user X exist" gets a
 * real, if trivially small, answer instead of a fabricated passwd entry for a
 * name that was never registered. */

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
