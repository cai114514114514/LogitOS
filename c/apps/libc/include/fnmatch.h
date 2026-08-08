#ifndef _FNMATCH_H
#define _FNMATCH_H

/* POSIX shell-style pattern matching. Pure computation, real and complete for
 * the standard wildcard grammar (*, ?, [...] with ranges/negation/classes),
 * checked byte-for-byte against glibc (tests/unit/libc_fnmatch_test.c). */

#define FNM_NOMATCH   1

#define FNM_NOESCAPE (1 << 0)   /* backslash is an ordinary character */
#define FNM_PATHNAME (1 << 1)   /* '/' in string must match '/' in pattern literally */
#define FNM_PERIOD   (1 << 2)   /* leading '.' must be matched explicitly */
#define FNM_CASEFOLD (1 << 3)   /* ASCII case-insensitive */
#define FNM_LEADING_DIR (1 << 4)        /* GNU: match ok at a '/' in string too */
#define FNM_FILE_NAME FNM_PATHNAME

int fnmatch(const char *pattern, const char *string, int flags);

#endif /* _FNMATCH_H */
