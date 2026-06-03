#ifndef AQUA_RNG_H
#define AQUA_RNG_H

#include <stdint.h>

void kernel_random_bytes(uint8_t *out, int len);

#endif /* AQUA_RNG_H */
