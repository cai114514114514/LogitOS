#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <stddef.h>
#include <sys/types.h>

/* These values are shared with SYS_GETRANDOM in logit_abi.h.  They are kept
 * literal here because installed libc headers cannot require the kernel's
 * private include tree; random.c has compile-time assertions against the ABI. */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int     getentropy(void *buf, size_t buflen);

#endif /* _SYS_RANDOM_H */
