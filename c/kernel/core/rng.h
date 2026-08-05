#ifndef AETHER_RNG_H
#define AETHER_RNG_H

#include <stdint.h>

void kernel_random_bytes(uint8_t *out, int len);

/* 1 if RDSEED/RDRAND is available (DRBG has a hardware entropy source). */
int rng_strong(void);

#endif /* AETHER_RNG_H */
