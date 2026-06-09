#ifndef AETHER_USERCOPY_H
#define AETHER_USERCOPY_H

#include <stdint.h>

int user_range_ok(const void *ptr, uint64_t len, int write);
int user_copy_from(void *dst, const void *src, uint64_t len);
int user_copy_to(void *dst, const void *src, uint64_t len);
int user_copy_string(char *dst, int max, const char *src);

#endif /* AETHER_USERCOPY_H */
