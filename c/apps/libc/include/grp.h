#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>

/* Groups, as far as this machine has them. There is no group DATABASE: an
 * account's gid equals its uid and a group's name is its account's name (see
 * /bin/login's enrolment and c/apps/coreutils/accounts.h), so getgrnam("alice")
 * works and getgrnam("wheel") does not. That is the truth rather than a
 * fabricated row. "root"/gid 0 exists whether or not any account has been
 * enrolled, because uid 0 is what the machine boots as.
 *
 * getgroups/setgroups are real kernel calls (SYS_GETGROUPS / SYS_SETGROUPS)
 * and the supplementary set they carry IS consulted by the filesystem's
 * permission check -- see vmeta_permission() in c/fs/vfs_meta.c. setgroups is
 * root-only, like every other credential change here. */

struct group {
    char  *gr_name;
    char  *gr_passwd;
    gid_t  gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result);
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result);
int getgroups(int size, gid_t list[]);
int setgroups(int size, const gid_t *list);

#endif /* _GRP_H */
