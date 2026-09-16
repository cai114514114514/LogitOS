/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int io_error(void)
{
    if (errno == EACCES || errno == EPERM || errno == ELOOP) {
        return AT_E_PERMISSION;
    }
    return AT_E_IO;
}

#if __STDC_HOSTED__
static int scoped_open(char *path, int flags)
{
    /* Hold each directory descriptor while acquiring its child. A separate
     * realpath check followed by open would have a rename/symlink race. Scoped
     * access deliberately rejects symlinks, including intermediate components;
     * unrestricted host access below keeps ordinary host path semantics. */
    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return -1;
    }
    char *component = path + 1;
    for (;;) {
        char *separator = strchr(component, '/');
        if (separator) {
            *separator = 0;
        }
        int wanted = separator ? O_RDONLY | O_DIRECTORY : flags;
        int child =
            openat(directory, *component ? component : ".", wanted | O_NOFOLLOW | O_CLOEXEC, 0666);
        int saved_errno = errno;
        close(directory);
        if (separator) {
            *separator = '/';
        }
        errno = saved_errno;
        if (child < 0 || !separator) {
            return child;
        }
        directory = child;
        component = separator + 1;
    }
}
#endif

int at_file_open(int *out, const char *path, int64_t length, int flags, uint32_t permission)
{
    *out = -1;
    if (!at_caps_have(permission)) {
        return AT_E_PERMISSION;
    }
    if (length <= 0 || length > AT_CAP_PATH_LIMIT || memchr(path, 0, (size_t)length)) {
        return AT_E_VALUE;
    }
    char requested[AT_CAP_PATH_LIMIT + 1];
    memcpy(requested, path, (size_t)length);
    requested[length] = 0;
    if (!at_caps_prefix()) {
        *out = open(requested, flags | O_CLOEXEC, 0666);
    } else {
        char absolute[AT_CAP_PATH_LIMIT + 1];
        const char *source = requested;
        if (requested[0] != '/') {
            if (!getcwd(absolute, sizeof absolute)) {
                return io_error();
            }
            size_t current = strlen(absolute);
            if (current + 1 + (size_t)length > AT_CAP_PATH_LIMIT) {
                return AT_E_VALUE;
            }
            absolute[current] = '/';
            memcpy(absolute + current + 1, requested, (size_t)length + 1);
            source = absolute;
        }
        char canonical[AT_CAP_PATH_LIMIT + 1];
        int status = at_caps_resolve_path(canonical, source, (int64_t)strlen(source));
        if (status) {
            return status;
        }
#if __STDC_HOSTED__
        *out = scoped_open(canonical, flags);
#else
        /* LogitFS has no symlinks or openat. The runtime scope check and the
         * kernel's actual capability ceiling both apply to this acquisition. */
        *out = open(canonical, flags | O_NOFOLLOW | O_CLOEXEC, 0666);
#endif
    }
    return *out < 0 ? io_error() : 0;
}

static int open_file(int *out, const char *path, int64_t length, int writing)
{
    int flags = writing ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY;
    uint32_t permission = writing ? AS_CAP_FS_WRITE : AS_CAP_FS_READ;
    return at_file_open(out, path, length, flags, permission);
}

int at_file_read(AtBytes **out, const char *path, int64_t length)
{
    *out = NULL;
    int descriptor;
    int status = open_file(&descriptor, path, length, 0);
    if (status) {
        return status;
    }
    unsigned char *data = NULL;
    size_t used = 0;
    size_t capacity = 0;
    for (;;) {
        unsigned char chunk[4096];
        ssize_t count = read(descriptor, chunk, sizeof chunk);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = io_error();
            break;
        }
        if (!count) {
            break;
        }
        if ((size_t)count > (size_t)AT_BUFFER_LIMIT - used) {
            status = AT_E_VALUE;
            break;
        }
        size_t needed = used + (size_t)count;
        if (needed > capacity) {
            size_t next = capacity ? capacity * 2 : sizeof chunk;
            if (next < needed) {
                next = needed;
            }
            unsigned char *grown = realloc(data, next);
            if (!grown) {
                status = AT_E_MEMORY;
                break;
            }
            data = grown;
            capacity = next;
        }
        memcpy(data + used, chunk, (size_t)count);
        used = needed;
        /* A short read is not EOF. Pipes and interrupted underlying drivers
         * may return a prefix even when the next read has more bytes. */
    }
    if (close(descriptor) < 0 && !status) {
        status = io_error();
    }
    if (!status) {
        /* Publish only after close succeeds. Native GC is not used while the
         * descriptor is live, and no partial buffer can escape on failure. */
        *out = at_bytes_copy(data, (int64_t)used);
        if (!*out) {
            status = AT_E_MEMORY;
        }
    }
    free(data);
    return status;
}

int at_file_write(int64_t *out, const char *path, int64_t length, const AtBytes *data)
{
    *out = 0;
    int descriptor;
    int status = open_file(&descriptor, path, length, 1);
    if (status) {
        return status;
    }
    int64_t written = 0;
    while (written < data->length) {
        size_t remaining = (size_t)(data->length - written);
        size_t chunk = remaining < 4096 ? remaining : 4096;
        ssize_t count = write(descriptor, data->data + written, chunk);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            status = count < 0 ? io_error() : AT_E_IO;
            break;
        }
        written += count;
    }
    /* Retrying close after EINTR can close an unrelated reused descriptor on
     * hosts that consumed it already. Report the failure, never retry blindly. */
    if (close(descriptor) < 0 && !status) {
        status = io_error();
    }
    if (!status) {
        *out = written;
    }
    return status;
}
