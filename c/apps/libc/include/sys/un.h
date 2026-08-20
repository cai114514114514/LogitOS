#ifndef _SYS_UN_H
#define _SYS_UN_H
#include <sys/socket.h>
#include <stddef.h>   /* offsetof, for SUN_LEN */
#include <string.h>   /* strlen,  for SUN_LEN */

/* <sys/un.h> -- the AF_UNIX address, and unlike most of this directory's
 * history it is backed by a kernel that implements it (c/net/core/unix.c).
 *
 * SUN_PATH IS 108 BYTES, which is not a style choice: a ported program is
 * entitled to write `char buf[sizeof(((struct sockaddr_un *)0)->sun_path)]`,
 * to memcpy a fixed-size name into it, and to pass `sizeof(struct sockaddr_un)`
 * as the length. Every one of those breaks if the number here disagrees with
 * Linux's, and the kernel ABI (struct logit_sockaddr_un in
 * include/abi/logit_abi.h) uses the same 108 for the same reason. THE TWO
 * STRUCTS ARE LAID OUT IDENTICALLY -- unsigned short, then 108 chars -- so the
 * conversion in c/apps/libc/src/socket.c is a copy and not a repack.
 *
 * WHERE THIS FILE LIVES, AND WHY IT CANNOT COLLIDE. `c/apps/libc/include/sys`
 * is one of the two directories the Makefile's INCDIRS explicitly filters out
 * (`filter-out %/include/sys %/include/uonly`), which exists because a
 * userland header whose basename the kernel also uses silently wins over the
 * kernel's -- the trap CLAUDE.md documents for sys/wait.h and sched.h. Checked
 * anyway before adding this: `un.h` occurs nowhere else under c/ or include/.
 *
 * THE ABSTRACT NAMESPACE (a sun_path whose first byte is NUL) IS NOT
 * SUPPORTED. bind() returns -1/EINVAL rather than treating the empty string as
 * a path -- see c/net/core/unix.c for the argument. There is deliberately no
 * constant here suggesting otherwise. */

#define UNIX_PATH_MAX 108

struct sockaddr_un {
    sa_family_t sun_family;          /* AF_UNIX */
    char        sun_path[UNIX_PATH_MAX];
};

/* Not POSIX, but glibc, the BSDs and a great deal of ported source have it:
 * the length of an address holding exactly this path. Offered because a
 * program that computes it by hand and gets it wrong passes a length the
 * kernel would have to guess about. */
#define SUN_LEN(p) (offsetof(struct sockaddr_un, sun_path) + strlen((p)->sun_path))

#endif /* _SYS_UN_H */
