#ifndef AETHER_RNG_H
#define AETHER_RNG_H

#include <stdint.h>

void kernel_random_bytes(uint8_t *out, int len);

#endif /* AETHER_RNG_H */
