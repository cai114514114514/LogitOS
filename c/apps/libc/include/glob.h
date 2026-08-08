#ifndef _GLOB_H
#define _GLOB_H
#include <stddef.h>

/* Filename expansion over <dirent.h> + <fnmatch.h>, both of which are real on
 * this system (opendir/readdir over SYS_DIR_COUNT/SYS_DIR_NAME). No brace
 * expansion (that is a shell feature, not glob(3)'s), no tilde expansion
 * unless GLOB_TILDE is passed (and even then: there is one user, one home,
 * see <pwd.h>), no GLOB_ALTDIRFUNC. */

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;

#define GLOB_ERR      (1 << 0)   /* stop and fail on a directory read error */
#define GLOB_MARK     (1 << 1)   /* append '/' to directory matches */
#define GLOB_NOSORT   (1 << 2)
#define GLOB_DOOFFS   (1 << 3)   /* leave gl_offs slots free at the front of gl_pathv */
#define GLOB_NOCHECK  (1 << 4)   /* no matches -> return the pattern itself */
#define GLOB_APPEND   (1 << 5)   /* append to a glob_t from a previous call */
#define GLOB_NOESCAPE (1 << 6)
#define GLOB_PERIOD   (1 << 7)   /* match a leading '.' with '*'/'?'/'[' */
#define GLOB_TILDE    (1 << 8)   /* expand a leading ~ (only "~" and "~root": one user) */
#define GLOB_BRACE    (1 << 9)   /* NOT SUPPORTED: rejected with GLOB_NOMATCH, not silently ignored */

#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3

int  glob(const char *pattern, int flags, int (*errfunc)(const char *epath, int eerrno), glob_t *pglob);
void globfree(glob_t *pglob);

#endif /* _GLOB_H */
