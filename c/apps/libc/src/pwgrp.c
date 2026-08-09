/* <pwd.h> / <grp.h>.
 *
 * WHAT CHANGED IN M32. This file used to be one hardcoded row -- "root", uid 0
 * -- and both headers explained at length why that was a real answer rather
 * than a stub, because the machine genuinely had one user. It now has an
 * account store (/etc/passwd; see c/apps/coreutils/accounts.h for the format
 * and for why it lives where it does), so this reads it.
 *
 * TWO THINGS A CALLER HAS TO KNOW, and neither is hidden:
 *
 * 1. THE STORE IS 0600 root:root, so an UNPRIVILEGED process cannot read it
 *    and getpwnam() returns NULL for every name including its own. That is not
 *    a bug and it is not worked around: the alternative is the /etc/shadow
 *    split, and accounts.h explains why this machine does not have a reader
 *    that needs the row without needing the hash. A program that wants to know
 *    who it is should call getuid(), which always works.
 *
 * 2. root IS STILL ALWAYS THERE, store or no store. uid 0 is not an account
 *    that was enrolled, it is the identity the machine boots as, and a
 *    getpwuid(0) that failed on a fresh image would be wrong.
 *
 * pw_passwd is reported as "x" and never as the hash, even when this code has
 * just read the hash. A struct passwd gets printed, logged and copied into
 * places nobody audits. */
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "logit_abi.h"      /* SYS_GETGROUPS / SYS_SETGROUPS / ID_NGROUPS_MAX */
#include "accounts.h"       /* the store format -- ONE parser, shared */

static char root_name[]   = "root";
static char pw_x[]        = "x";
static char root_gecos[]  = "root";
static char root_dir[]    = "/";
static char def_shell[]   = "/bin/sh";

static struct passwd root_pw = {
    .pw_name = root_name, .pw_passwd = pw_x, .pw_uid = 0, .pw_gid = 0,
    .pw_gecos = root_gecos, .pw_dir = root_dir, .pw_shell = def_shell,
};

/* One row's strings, owned by this file, POSIX-style: the returned pointer is
 * valid until the next call. */
static struct passwd cur_pw;
static char cur_name[ACCT_NAME], cur_home[ACCT_PATH], cur_shell[ACCT_PATH];

static void fill(const struct account *a)
{
    strncpy(cur_name,  a->name,  sizeof cur_name  - 1); cur_name[sizeof cur_name - 1]   = 0;
    strncpy(cur_home,  a->home,  sizeof cur_home  - 1); cur_home[sizeof cur_home - 1]   = 0;
    strncpy(cur_shell, a->shell, sizeof cur_shell - 1); cur_shell[sizeof cur_shell - 1] = 0;
    cur_pw.pw_name   = cur_name;
    cur_pw.pw_passwd = pw_x;                 /* never the hash. See above. */
    cur_pw.pw_uid    = a->uid;
    cur_pw.pw_gid    = a->gid;
    cur_pw.pw_gecos  = cur_name;
    cur_pw.pw_dir    = cur_home;
    cur_pw.pw_shell  = cur_shell;
}

/* The store, re-read on each setpwent()/lookup burst. Cached for the duration
 * of one getpwent() walk only -- a login that happens between two lookups
 * should be visible to the second. */
static char pw_store[ACCT_MAX + 1];
static int  pw_store_len = -1;

static int load(void)
{
    pw_store_len = 0;
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) { errno = 0; return 0; }     /* absent OR refused: no rows */
    ssize_t n = read(fd, pw_store, ACCT_MAX);
    close(fd);
    if (n <= 0) { errno = 0; return 0; }
    pw_store_len = (int)n;
    pw_store[n] = 0;
    return 1;
}

struct passwd *getpwuid(uid_t uid)
{
    if (uid == 0) return &root_pw;
    load();
    struct account a; int pos = 0;
    while (acct_next(pw_store, pw_store_len, &pos, &a))
        if (a.uid == (unsigned)uid) { fill(&a); return &cur_pw; }
    errno = 0;
    return (struct passwd *)0;
}

struct passwd *getpwnam(const char *name)
{
    if (name && strcmp(name, "root") == 0) return &root_pw;
    if (!name) { errno = 0; return (struct passwd *)0; }
    load();
    struct account a;
    if (acct_find(pw_store, pw_store_len, name, &a)) { fill(&a); return &cur_pw; }
    errno = 0;
    return (struct passwd *)0;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    (void)buf; (void)buflen;
    struct passwd *p = getpwuid(uid);
    if (!p) { *result = 0; return 0; }
    *pwd = *p; *result = pwd;
    return 0;
}
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    (void)buf; (void)buflen;
    struct passwd *p = getpwnam(name);
    if (!p) { *result = 0; return 0; }
    *pwd = *p; *result = pwd;
    return 0;
}

/* getpwent() walks root first (it is in no file) and then the store. */
static int pw_cursor;       /* 0 = before root, 1 = walking the store */
static int pw_pos;
struct passwd *getpwent(void)
{
    if (!pw_cursor) { pw_cursor = 1; pw_pos = 0; load(); return &root_pw; }
    struct account a;
    if (acct_next(pw_store, pw_store_len, &pw_pos, &a)) { fill(&a); return &cur_pw; }
    return 0;
}
void setpwent(void) { pw_cursor = 0; pw_pos = 0; }
void endpwent(void) { pw_cursor = 0; pw_pos = 0; }

/* --- groups ---------------------------------------------------------------
 * There is no group DATABASE and this file does not invent one: an account's
 * gid equals its uid (see /bin/login's enrolment) and a group's name is its
 * account's name. getgrnam("alice") therefore works and getgrnam("wheel")
 * does not, which is the truth rather than a fabricated row. */

static char *root_members[] = { root_name, (char *)0 };
static struct group root_gr = { .gr_name = root_name, .gr_passwd = pw_x, .gr_gid = 0, .gr_mem = root_members };

static struct group cur_gr;
static char *cur_members[2];

static struct group *group_of(const struct passwd *p)
{
    cur_members[0] = p->pw_name; cur_members[1] = 0;
    cur_gr.gr_name = p->pw_name; cur_gr.gr_passwd = pw_x;
    cur_gr.gr_gid = p->pw_gid; cur_gr.gr_mem = cur_members;
    return &cur_gr;
}

struct group *getgrgid(gid_t gid)
{
    if (gid == 0) return &root_gr;
    load();
    struct account a; int pos = 0;
    while (acct_next(pw_store, pw_store_len, &pos, &a))
        if (a.gid == (unsigned)gid) { fill(&a); return group_of(&cur_pw); }
    errno = 0;
    return (struct group *)0;
}
struct group *getgrnam(const char *name)
{
    if (name && strcmp(name, "root") == 0) return &root_gr;
    struct passwd *p = getpwnam(name);
    if (!p) { errno = 0; return (struct group *)0; }
    return group_of(p);
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    (void)buf; (void)buflen;
    struct group *g = getgrgid(gid);
    if (!g) { *result = 0; return 0; }
    *grp = *g; *result = grp;
    return 0;
}
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    (void)buf; (void)buflen;
    struct group *g = getgrnam(name);
    if (!g) { *result = 0; return 0; }
    *grp = *g; *result = grp;
    return 0;
}

/* The calling process's supplementary groups -- a real kernel query now
 * (SYS_GETGROUPS), not the constant 0 this used to return. size == 0 is the
 * POSIX "just tell me the count" probe and never fails for want of room. */
static long id_sys(long n, long a, long b)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(0L) : "memory"); return r; }

int getgroups(int size, gid_t list[])
{
    if (size < 0) { errno = EINVAL; return -1; }
    unsigned tmp[ID_NGROUPS_MAX];
    long n = id_sys(SYS_GETGROUPS, 0, 0);             /* how many? */
    if (n < 0) { errno = EINVAL; return -1; }
    if (size == 0) return (int)n;
    if (n > size) { errno = EINVAL; return -1; }      /* POSIX: not truncation */
    if (n == 0) return 0;
    if (id_sys(SYS_GETGROUPS, (long)ID_NGROUPS_MAX, (long)tmp) < 0) { errno = EINVAL; return -1; }
    for (long i = 0; i < n; i++) list[i] = (gid_t)tmp[i];
    return (int)n;
}

int setgroups(int size, const gid_t *list)
{
    if (size < 0 || size > ID_NGROUPS_MAX) { errno = EINVAL; return -1; }
    unsigned tmp[ID_NGROUPS_MAX];
    for (int i = 0; i < size; i++) tmp[i] = (unsigned)list[i];
    if (id_sys(SYS_SETGROUPS, size, (long)tmp) < 0) { errno = EPERM; return -1; }
    return 0;
}
