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
#define NAME_MAX    63
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
#ifndef MB_LEN_MAX
#define MB_LEN_MAX 4          /* UTF-8; see <wchar.h> */
#endif
#ifndef NZERO
#define NZERO 20
#endif

#endif /* _LIBC_LIMITS_H */
