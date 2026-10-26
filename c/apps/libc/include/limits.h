#ifndef _LIBC_LIMITS_H
#define _LIBC_LIMITS_H

/* The compiler owns the C limits (CHAR_BIT, INT_MAX, LLONG_MIN, ...): a
 * freestanding implementation is entitled to clang's <limits.h> and clang gets
 * them right for x86_64-elf, so this header adds to it rather than replacing
 * it. #include_next resumes the search after this directory. */
#include_next <limits.h>

/* POSIX limits, which live in <limits.h> and which clang's does not provide.
 * The values are LogitOS's real ones, not aspirational: PATH_MAX and NAME_MAX
 * match what c/fs/logitfs.c will actually accept, so a buffer sized by this
 * header is a buffer the filesystem cannot overrun. */
#ifndef PATH_MAX
#define PATH_MAX   256
#endif
#ifndef NAME_MAX
#define NAME_MAX    59        /* LFS_NAME_MAX (60, the dirent's name field) minus its NUL:
                               * VFS_NAME_MAX in c/fs/vfs/vfs_path.h, which
                               * tests/unit/libc_name_max_test.c pins this to. It was
                               * 63 and this comment promised it matched what the
                               * filesystem accepts; a 60-character name is refused. */
#endif
#ifndef LINK_MAX
#define LINK_MAX     1        /* LogitFS has no hard links */
#endif
#ifndef OPEN_MAX
#define OPEN_MAX    32        /* c/kernel/exec/proc.c fd table */
#endif
#ifndef ARG_MAX
#define ARG_MAX   4096
#endif
#ifndef PIPE_BUF
#define PIPE_BUF  4096
#endif
#ifndef NGROUPS_MAX
#define NGROUPS_MAX 0         /* no users, no groups */
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif
/* UNCONDITIONAL, and that is the fix. clang's <limits.h> (pulled in above)
 * already defines MB_LEN_MAX as 1, so the `#ifndef` form this used to be never
 * took effect: programs saw 1 while MB_CUR_MAX is 4, and the standard idiom
 * `char b[MB_LEN_MAX]; wcrtomb(b, wc, &st);` overran b by up to three bytes on
 * any non-ASCII character. The standard requires MB_LEN_MAX >= MB_CUR_MAX. */
#undef MB_LEN_MAX
#define MB_LEN_MAX 4          /* UTF-8; see <wchar.h> */
#ifndef NZERO
#define NZERO 20
#endif

#endif /* _LIBC_LIMITS_H */
