#ifndef LOGIT_RNG_H
#define LOGIT_RNG_H

#include <stdint.h>

void kernel_random_bytes(uint8_t *out, int len);

/* 1 if RDSEED/RDRAND is available (DRBG has a hardware entropy source). */
int rng_strong(void);

/* SYS_GETRANDOM back end. (buf, len, flags) -> bytes written, or -1.
 * buf == NULL is the "is the DRBG hardware-backed?" query and returns
 * rng_strong(). Never returns a SHORT count: see the long comment in rng.c. */
long rng_syscall(long ubuf, long len, long flags);

#endif /* LOGIT_RNG_H */
