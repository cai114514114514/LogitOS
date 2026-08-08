#ifndef EXECHOST_RNG_H
#define EXECHOST_RNG_H
#include <stdint.h>
void kernel_random_bytes(uint8_t *out, int len);
#endif
