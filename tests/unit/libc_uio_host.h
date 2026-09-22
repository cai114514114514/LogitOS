#ifndef LIBC_UIO_HOST_H
#define LIBC_UIO_HOST_H

#include <stddef.h>

/* Setup only, compiled with HOST headers in its own translation unit. Neither
 * side may pass LogitOS/Linux flag numbers to a different host's open/fcntl. */
int host_temp_file(const void *contents, size_t length, int writable);
int host_full_pipe(int descriptors[2]);

#endif
