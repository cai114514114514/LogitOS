#ifndef LOGIT_POWER_EC_TRANSPORT_H
#define LOGIT_POWER_EC_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

enum ec_result {
    EC_OK = 0,
    EC_INVALID = -1,
    EC_IO_ERROR = -2,
    EC_TIMEOUT = -3,
    EC_PROTOCOL = -4,
    EC_QUARANTINED = -5,
};

/* Logical ports deliberately hide any conventional 0x62/0x66 assumption.
 * The binding layer owns the resources and maps these to firmware addresses. */
enum ec_port { EC_DATA_PORT, EC_CONTROL_PORT };
struct ec_transport_ops {
    void *context;
    int (*lock)(void *context);
    void (*unlock)(void *context);
    int (*read)(void *context, enum ec_port port, uint8_t *value);
    int (*write)(void *context, enum ec_port port, uint8_t value);
    uint64_t (*now_us)(void *context);
    void (*delay_us)(void *context, unsigned microseconds);
};

struct ec_transport {
    struct ec_transport_ops ops;
    unsigned timeout_us;
    int quarantined;
    int last_error;
    uint64_t completed;
};

int ec_transport_init(struct ec_transport *transport,
                      const struct ec_transport_ops *ops, unsigned timeout_us);
/* Width 1..8, little endian, entirely inside the 256-byte EC address space.
 * Reads commit output only after every byte completed. Writes cannot be rolled
 * back; an error may follow a completed prefix. No automatic write retry. */
int ec_transport_read(struct ec_transport *transport, unsigned address, unsigned width,
                      uint64_t *value);
int ec_transport_write(struct ec_transport *transport, unsigned address, unsigned width,
                       uint64_t value);
/* Only issue QR_EC when SCI_EVT is set. A successful result of zero means the
 * controller explicitly reported no event (or SCI_EVT was not asserted). */
int ec_transport_query(struct ec_transport *transport, uint8_t *query);
#endif
