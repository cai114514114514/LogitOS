/* Real host file/pipe setup shared by both differential binaries. No vector
 * operation is implemented here: readv/writev/preadv/pwritev remain the APIs
 * being compared. Compile separately, WITHOUT LIBC_HOST_INC/-nostdinc. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "libc_uio_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int host_temp_file(const void *contents, size_t length, int writable)
{
    char path[] = "/tmp/logit-uio-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    const char *bytes = contents;
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, bytes + written, length - written);
        if (count <= 0) { close(fd); unlink(path); return -1; }
        written += (size_t)count;
    }
    if (!writable) {
        int readable = open(path, O_RDONLY);
        close(fd);
        fd = readable;
    }
    unlink(path);
    if (fd >= 0 && lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    return fd;
}

int host_full_pipe(int descriptors[2])
{
    if (pipe(descriptors) < 0) return -1;
    int flags = fcntl(descriptors[1], F_GETFL);
    if (flags < 0 || fcntl(descriptors[1], F_SETFL, flags | O_NONBLOCK) < 0)
        goto failed;

    /* F_SETPIPE_SZ is Linux-only. Fill the actual pipe instead of asserting a
     * guessed capacity. Switch to one byte after a refused block, since a
     * pipe may still have less than PIPE_BUF bytes available. */
    char data[4096];
    memset(data, 'F', sizeof data);
    size_t total = 0, request = sizeof data;
    while (total < 32u * 1024u * 1024u) {
        ssize_t count = write(descriptors[1], data, request);
        if (count > 0) {
            total += (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (request == 1) return 0;
            request = 1;
            continue;
        }
        goto failed;
    }
failed:
    close(descriptors[0]);
    close(descriptors[1]);
    return -1;
}
