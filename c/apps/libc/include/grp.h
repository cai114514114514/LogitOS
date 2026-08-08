#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>

/* One group, matching <pwd.h>'s one user: "root", gid 0, whose only member is
 * "root". See the note there for why this is a real (if minimal) answer and
 * not a stub. */

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

#endif /* _GRP_H */
