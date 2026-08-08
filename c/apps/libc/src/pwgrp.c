/* <pwd.h> / <grp.h>: the one account this machine has. See both headers for
 * why "root"/gid 0 is a real answer rather than an invented one. */
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <errno.h>

static char root_name[]   = "root";
static char root_passwd[] = "x";
static char root_gecos[]  = "root";
static char root_dir[]    = "/";
static char root_shell[]  = "/bin/sh";

static struct passwd root_pw = {
    .pw_name = root_name, .pw_passwd = root_passwd, .pw_uid = 0, .pw_gid = 0,
    .pw_gecos = root_gecos, .pw_dir = root_dir, .pw_shell = root_shell,
};

struct passwd *getpwuid(uid_t uid) { return uid == 0 ? &root_pw : (errno = 0, (struct passwd *)0); }
struct passwd *getpwnam(const char *name)
{ return (name && strcmp(name, "root") == 0) ? &root_pw : (errno = 0, (struct passwd *)0); }

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    (void)buf; (void)buflen;
    if (uid != 0) { *result = 0; return 0; }
    *pwd = root_pw; *result = pwd;
    return 0;
}
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    (void)buf; (void)buflen;
    if (!name || strcmp(name, "root") != 0) { *result = 0; return 0; }
    *pwd = root_pw; *result = pwd;
    return 0;
}

/* getpwent() walks "the" passwd database, which has exactly one row; the
 * cursor is which side of it setpwent()/endpwent() left the caller on. */
static int pw_cursor;
struct passwd *getpwent(void) { if (pw_cursor) return 0; pw_cursor = 1; return &root_pw; }
void setpwent(void) { pw_cursor = 0; }
void endpwent(void) { pw_cursor = 0; }

static char *root_members[] = { root_name, (char *)0 };
static struct group root_gr = { .gr_name = root_name, .gr_passwd = root_passwd, .gr_gid = 0, .gr_mem = root_members };

struct group *getgrgid(gid_t gid) { return gid == 0 ? &root_gr : (errno = 0, (struct group *)0); }
struct group *getgrnam(const char *name)
{ return (name && strcmp(name, "root") == 0) ? &root_gr : (errno = 0, (struct group *)0); }

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    (void)buf; (void)buflen;
    if (gid != 0) { *result = 0; return 0; }
    *grp = root_gr; *result = grp;
    return 0;
}
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    (void)buf; (void)buflen;
    if (!name || strcmp(name, "root") != 0) { *result = 0; return 0; }
    *grp = root_gr; *result = grp;
    return 0;
}

/* The calling process's supplementary groups: none, because there is one
 * group and it is the primary one. size==0 is the POSIX "just tell me the
 * count" probe. */
int getgroups(int size, gid_t list[])
{
    (void)list;
    if (size < 0) { errno = EINVAL; return -1; }
    return 0;
}
