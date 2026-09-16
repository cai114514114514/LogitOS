/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_PORT_H
#define AS_NATIVE_PORT_H
#include "native.h"

/* Port memory is owned by generated with cleanup, not by the tracing GC.
 * Read buffers contain bytes only. Returned Bytes/text have independent GC
 * storage and remain valid after the descriptor and owner are released. */
typedef struct AtPort AtPort;

enum AtPortMethod {
    AT_PORT_READ,
    AT_PORT_READ_EXACT,
    AT_PORT_READ_ALL,
    AT_PORT_WRITE,
    AT_PORT_LINE,
    AT_PORT_CLOSE,
    AT_PORT_CLOSED,
    AT_PORT_FD,
    AT_PORT_KIND,
    AT_PORT_LINES
};

int at_port_open(AtPort **out, const char *path, int64_t path_length, const char *mode,
                 int64_t mode_length);
/* The wrapper is scope-owned, but the existing descriptor remains borrowed.
 * Closing/releasing it must not close a caller's standard stream or file. */
int at_port_borrow(AtPort **out, int64_t descriptor);
/* Publish both ends only after complete acquisition; failure leaves both NULL. */
int at_port_pipe(AtPort **reader, AtPort **writer);
void at_port_kind(AtNativeText *out, const AtPort *port);
int at_port_close(AtPort *port);
int at_port_release(AtPort *port);
int at_port_read(AtBytes **out, AtPort *port, int64_t count, int exact);
int at_port_readall(AtBytes **out, AtPort *port);
int at_port_line(AtNativeText *out, int32_t *present, AtPort *port);
int at_port_write(int64_t *out, AtPort *port, const void *bytes, int64_t length);
int64_t at_port_fd(const AtPort *port);
int32_t at_port_closed(const AtPort *port);
/* Internal measurement for native runtime tests, not a fabricated fd count. */
int64_t at_port_live(void);

typedef struct {
    int64_t open;
    int64_t closed;
    int64_t close_errors;
} AtPortCounters;

/* Counters describe owned Port descriptors, not every fd in the process. */
void at_port_counters(AtPortCounters *out);
AtDict *at_port_stats(AtHash hash, AtEqual equal);
#endif
