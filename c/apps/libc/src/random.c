/* Public entropy APIs over the kernel Hash_DRBG.  This is deliberately a new
 * TU instead of another clock-seeded generator in stdlib.c: getrandom,
 * getentropy and arc4random must all have one cryptographic source and one
 * refusal boundary. */
#include <sys/random.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include "logit_abi.h"

_Static_assert(GRND_NONBLOCK == 0x0001, "libc/kernel GRND_NONBLOCK drift");
_Static_assert(GRND_RANDOM == 0x0002, "libc/kernel GRND_RANDOM drift");

static long random_sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) {
        errno = EINVAL;
        return -1;
    }
    if (buflen > (size_t)SSIZE_MAX) { errno = EINVAL; return -1; }
    if (!buf && buflen) { errno = EFAULT; return -1; }

    /* SYS_GETRANDOM intentionally caps one entry at GRND_MAX so the kernel
     * never holds its RNG spinlock for an unbounded request.  The libc wrapper
     * owns the loop: otherwise a caller that reasonably asks for 4097 bytes
     * gets a silently zero tail when it checks only for -1, the exact failure
     * the ABI comment warns about. */
    unsigned char *p = buf;
    size_t done = 0;
    while (done < buflen) {
        size_t ask = buflen - done;
        if (ask > GRND_MAX) ask = GRND_MAX;
        long n = random_sys(SYS_GETRANDOM, (long)(p + done), (long)ask, (long)flags);
        /* With length and flags checked above, this kernel's only remaining
         * -1 path is a user range it cannot write. Preserve any completed
         * chunks just like a short system read; otherwise expose EFAULT. */
        if (n <= 0) {
            errno = EFAULT;
            return done ? (ssize_t)done : -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

int getentropy(void *buf, size_t buflen)
{
    /* POSIX/OpenBSD fix this ceiling at 256 bytes so callers cannot mistake a
     * bulk random stream for one entropy seed. */
    if (buflen > 256) { errno = EIO; return -1; }
    return getrandom(buf, buflen, 0) == (ssize_t)buflen ? 0 : -1;
}

void arc4random_buf(void *buf, size_t n)
{
    if (getrandom(buf, n, 0) != (ssize_t)n) abort();
}

uint32_t arc4random(void)
{
    uint32_t value;
    arc4random_buf(&value, sizeof value);
    return value;
}

uint32_t arc4random_uniform(uint32_t upper_bound)
{
    if (upper_bound < 2) return 0;
    /* Values below min would make the final modulo slightly favour the low
     * residues.  Rejection gives every result exactly the same preimage count. */
    uint32_t min = (uint32_t)(-upper_bound) % upper_bound;
    uint32_t value;
    do value = arc4random(); while (value < min);
    return value % upper_bound;
}
