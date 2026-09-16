/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "port.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct AtPort {
    int descriptor;
    int readable, writable;
    int borrowed, terminal, pipe_endpoint;
    int eof;
    unsigned char buffer[4096];
    size_t position, used;
};

static int64_t live_ports;
static int64_t closed_ports;
static int64_t close_errors;

static int io_error(void)
{
    return errno == EACCES || errno == EPERM ? AT_E_PERMISSION : AT_E_IO;
}

static int mode_is(const char *text, int64_t length, const char *wanted)
{
    return length == (int64_t)strlen(wanted) && !memcmp(text, wanted, (size_t)length);
}

int at_port_open(AtPort **out, const char *path, int64_t path_length, const char *mode,
                 int64_t mode_length)
{
    *out = NULL;
    int flags, readable = 0, writable = 0;
    if (mode_is(mode, mode_length, "r") || mode_is(mode, mode_length, "rb")) {
        flags = O_RDONLY;
        readable = 1;
    } else if (mode_is(mode, mode_length, "w") || mode_is(mode, mode_length, "wb")) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        writable = 1;
    } else if (mode_is(mode, mode_length, "a") || mode_is(mode, mode_length, "ab")) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        writable = 1;
    } else if (mode_is(mode, mode_length, "rw")) {
        flags = O_RDWR | O_CREAT;
        readable = writable = 1;
    } else if (mode_is(mode, mode_length, "r+b")) {
        /* Preserve the existing rw create behavior, while r+b explicitly
         * requires an existing file as its spelling promises. */
        flags = O_RDWR;
        readable = writable = 1;
    } else {
        return AT_E_VALUE;
    }
    /* Allocate before acquisition so an allocation failure cannot orphan an
     * already-open fd. The GC never owns this object or its descriptor. */
    AtPort *port = calloc(1, sizeof *port);
    if (!port) {
        return AT_E_MEMORY;
    }
    uint32_t permission = (readable ? AS_CAP_FS_READ : 0) | (writable ? AS_CAP_FS_WRITE : 0);
    int status = at_file_open(&port->descriptor, path, path_length, flags, permission);
    if (status) {
        free(port);
        return status;
    }
    port->readable = readable;
    port->writable = writable;
    live_ports++;
    *out = port;
    return 0;
}

int at_port_close(AtPort *port)
{
    if (!port || port->descriptor < 0) {
        return 0;
    }
    int descriptor = port->descriptor;
    /* Consume ownership before close. Even EINTR must not make cleanup retry
     * a descriptor that the host might already have reassigned to another file. */
    port->descriptor = -1;
    port->position = port->used = 0;
    port->eof = 1;
    if (port->borrowed) {
        return 0;
    }
    live_ports--;
    int status = close(descriptor) < 0 ? io_error() : 0;
    if (status) {
        close_errors++;
    } else {
        closed_ports++;
    }
    return status;
}

int at_port_borrow(AtPort **out, int64_t descriptor)
{
    *out = NULL;
    if (descriptor < 0 || descriptor > 65535) {
        return AT_E_VALUE;
    }
    /* Preserve the inherited-stdio boundary from A2: naming 0/1/2 adds no
     * authority, while arbitrary descriptor numbers require CAP_RAW. This
     * does not infer a filesystem grant from an untrusted descriptor number. */
    if (descriptor > 2 && !at_caps_have(AS_CAP_RAW)) {
        return AT_E_PERMISSION;
    }
    AtPort *port = calloc(1, sizeof *port);
    if (!port) {
        return AT_E_MEMORY;
    }
    port->descriptor = (int)descriptor;
    /* Mini-libc F_GETFL returns O_RDWR without querying the kernel, even for
     * an absent fd. Do not treat it as validation. The real read/write calls
     * report absent descriptors and forbidden directions, on both platforms. */
    port->readable = port->writable = 1;
    port->borrowed = 1;
    /* "tty" is the historical API classification of inherited 0/1/2, even
     * when the launcher redirected them to a file or pipe. It is not isatty. */
    port->terminal = descriptor <= 2;
    *out = port;
    return 0;
}

void at_port_kind(AtNativeText *out, const AtPort *port)
{
    if (port->pipe_endpoint) {
        *out = (AtNativeText){"pipe", 4};
    } else {
        *out = port->terminal ? (AtNativeText){"tty", 3} : (AtNativeText){"file", 4};
    }
}

int at_port_pipe(AtPort **reader, AtPort **writer)
{
    *reader = NULL;
    *writer = NULL;
    if (!at_caps_have(AS_CAP_PROC)) {
        return AT_E_PERMISSION;
    }

    /* Allocate both wrappers before opening either descriptor. Generated
     * cleanup becomes active only after success, so this function must leave
     * no partial owner behind on allocation or kernel pipe failure. */
    AtPort *input = calloc(1, sizeof *input);
    if (!input) {
        return AT_E_MEMORY;
    }
    AtPort *output = calloc(1, sizeof *output);
    if (!output) {
        free(input);
        return AT_E_MEMORY;
    }
    int descriptors[2];
    if (pipe(descriptors) < 0) {
        int status = io_error();
        free(output);
        free(input);
        return status;
    }
    input->descriptor = descriptors[0];
    input->readable = 1;
    input->pipe_endpoint = 1;
    output->descriptor = descriptors[1];
    output->writable = 1;
    output->pipe_endpoint = 1;
    live_ports += 2;
    *reader = input;
    *writer = output;
    return 0;
}

int at_port_release(AtPort *port)
{
    int status = at_port_close(port);
    free(port);
    return status;
}

int64_t at_port_fd(const AtPort *port)
{
    return port->descriptor;
}

int32_t at_port_closed(const AtPort *port)
{
    return port->descriptor < 0;
}

int64_t at_port_live(void)
{
    return live_ports;
}

void at_port_counters(AtPortCounters *out)
{
    *out = (AtPortCounters){live_ports, closed_ports, close_errors};
}

static int fill(AtPort *port, size_t requested)
{
    if (port->position < port->used || port->eof) {
        return 0;
    }
    ssize_t count;
    size_t capacity = sizeof port->buffer;
    /* A short-lived borrowed wrapper cannot retain unread bytes for the next
     * user of the descriptor. Read only this operation's requested extent;
     * line() requests one byte, since a pipe cannot rewind past a newline.
     * Owned ports retain their normal 4 KiB buffering across operations. */
    if (port->borrowed && requested < capacity) {
        capacity = requested;
    }
    do {
        count = read(port->descriptor, port->buffer, capacity);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        return io_error();
    }
    port->position = 0;
    port->used = (size_t)count;
    port->eof = count == 0;
    return 0;
}

int at_port_read(AtBytes **out, AtPort *port, int64_t count, int exact)
{
    *out = NULL;
    if (count <= 0 || count > AT_BUFFER_LIMIT) {
        return AT_E_VALUE;
    }
    if (port->descriptor < 0) {
        return exact ? AT_E_IO : 0;
    }
    if (!port->readable) {
        return AT_E_IO;
    }
    unsigned char *data = malloc((size_t)count);
    if (!data) {
        return AT_E_MEMORY;
    }
    size_t used = 0;
    int status = 0;
    while (used < (size_t)count) {
        status = fill(port, (size_t)count - used);
        if (status || port->eof) {
            break;
        }
        size_t chunk = port->used - port->position;
        if (chunk > (size_t)count - used) {
            chunk = (size_t)count - used;
        }
        memcpy(data + used, port->buffer + port->position, chunk);
        used += chunk;
        port->position += chunk;
    }
    if (!status && exact && used != (size_t)count) {
        status = AT_E_IO;
    }
    if (!status && used) {
        *out = at_bytes_copy(data, (int64_t)used);
        if (!*out) {
            status = AT_E_MEMORY;
        }
    }
    free(data);
    return status;
}

static int append(unsigned char **data, size_t *used, size_t *capacity, const void *source,
                  size_t length)
{
    if (length > (size_t)AT_BUFFER_LIMIT - *used) {
        return AT_E_VALUE;
    }
    size_t needed = *used + length;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity * 2 : 4096;
        if (next < needed) {
            next = needed;
        }
        unsigned char *grown = realloc(*data, next);
        if (!grown) {
            return AT_E_MEMORY;
        }
        *data = grown;
        *capacity = next;
    }
    if (length) {
        memcpy(*data + *used, source, length);
    }
    *used = needed;
    return 0;
}

int at_port_readall(AtBytes **out, AtPort *port)
{
    *out = NULL;
    if (port->descriptor >= 0 && !port->readable) {
        return AT_E_IO;
    }
    unsigned char *data = NULL;
    size_t used = 0, capacity = 0;
    int status = 0;
    while (port->descriptor >= 0 && !port->eof) {
        status = fill(port, sizeof port->buffer);
        if (status) {
            break;
        }
        status = append(&data, &used, &capacity, port->buffer + port->position,
                        port->used - port->position);
        if (status) {
            break;
        }
        port->position = port->used;
    }
    if (!status) {
        *out = at_bytes_copy(data, (int64_t)used);
        if (!*out) {
            status = AT_E_MEMORY;
        }
    }
    free(data);
    return status;
}

int at_port_line(AtNativeText *out, int32_t *present, AtPort *port)
{
    *out = (AtNativeText){0};
    *present = 0;
    if (port->descriptor < 0) {
        return 0;
    }
    if (!port->readable) {
        return AT_E_IO;
    }
    unsigned char *data = NULL;
    size_t used = 0, capacity = 0;
    int status = 0, found = 0;
    while (!port->eof) {
        status = fill(port, 1);
        if (status || port->eof) {
            break;
        }
        const unsigned char *start = port->buffer + port->position;
        size_t available = port->used - port->position;
        const unsigned char *newline = memchr(start, '\n', available);
        size_t length = newline ? (size_t)(newline - start) : available;
        status = append(&data, &used, &capacity, start, length);
        if (status) {
            break;
        }
        port->position += length + (newline != NULL);
        if (newline) {
            found = 1;
            break;
        }
    }
    if (!status && (found || used)) {
        if (used && data[used - 1] == '\r') {
            used--;
        }
        AtBytes *bytes = at_bytes_copy(data, (int64_t)used);
        if (!bytes) {
            status = AT_E_MEMORY;
        } else {
            /* decode does not allocate; its returned interior pointer keeps
             * this Bytes owner alive once the caller stores its text root. */
            status = at_bytes_decode(out, bytes);
            *present = status == 0;
        }
    }
    free(data);
    return status;
}

int at_port_write(int64_t *out, AtPort *port, const void *bytes, int64_t length)
{
    *out = 0;
    if (port->descriptor < 0 || !port->writable) {
        return AT_E_IO;
    }
    if (length < 0 || length > AT_BUFFER_LIMIT) {
        return AT_E_VALUE;
    }
    /* Buffered reads can advance the underlying fd beyond the logical
     * cursor. Reconcile it before writing through a read/write file. */
    size_t unread = port->used - port->position;
    if (unread && lseek(port->descriptor, -(off_t)unread, SEEK_CUR) < 0) {
        return io_error();
    }
    port->position = port->used = 0;
    port->eof = 0;
    while (*out < length) {
        size_t left = (size_t)(length - *out);
        ssize_t count =
            write(port->descriptor, (const unsigned char *)bytes + *out, left > 4096 ? 4096 : left);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return count < 0 ? io_error() : AT_E_IO;
        }
        *out += count;
    }
    return 0;
}
