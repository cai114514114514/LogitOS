#include "transport.h"

enum {
    EC_STATUS_OBF = 1u << 0,
    EC_STATUS_IBF = 1u << 1,
    EC_STATUS_SCI_EVT = 1u << 5,
    EC_COMMAND_READ = 0x80,
    EC_COMMAND_WRITE = 0x81,
    EC_COMMAND_QUERY = 0x84,
    EC_ADDRESS_BYTES = 256,
    EC_MAX_TRANSFER_BYTES = 8,
    EC_POLL_US = 10,
    EC_MAX_TIMEOUT_US = 1000000,
};

int ec_transport_init(struct ec_transport *transport,
                      const struct ec_transport_ops *ops, unsigned timeout_us)
{
    if (!transport || !ops || !ops->lock || !ops->unlock || !ops->read || !ops->write ||
        !ops->now_us || !ops->delay_us || !timeout_us || timeout_us > EC_MAX_TIMEOUT_US)
        return EC_INVALID;
    *transport = (struct ec_transport){.ops = *ops, .timeout_us = timeout_us};
    return EC_OK;
}

/* Both a monotonic deadline and an iteration ceiling bound a broken timer.
 * The deadline is per handshake, not an assumption about CPU instruction rate. */
static int wait_status(struct ec_transport *transport, unsigned mask, unsigned expected,
                       uint8_t *last_status)
{
    const struct ec_transport_ops *ops = &transport->ops;
    uint64_t started_at = ops->now_us(ops->context);
    unsigned attempts = transport->timeout_us / EC_POLL_US + 2;
    while (attempts--) {
        uint8_t status;
        if (ops->read(ops->context, EC_CONTROL_PORT, &status))
            return EC_IO_ERROR;
        if (last_status)
            *last_status = status;
        if ((status & mask) == expected)
            return EC_OK;
        if (ops->now_us(ops->context) - started_at >= transport->timeout_us)
            break;
        ops->delay_us(ops->context, EC_POLL_US);
    }
    return EC_TIMEOUT;
}

static int wait_input_empty(struct ec_transport *transport)
{
    return wait_status(transport, EC_STATUS_IBF, 0, NULL);
}

static int send_byte(struct ec_transport *transport, enum ec_port port, uint8_t value)
{
    uint8_t flags;
    int status = wait_status(transport, EC_STATUS_IBF, 0, &flags);
    if (status)
        return status;
    if (flags & EC_STATUS_OBF)
        return EC_PROTOCOL;
    if (transport->ops.write(transport->ops.context, port, value))
        return EC_IO_ERROR;

    /* ACPI 12.7 allows up to 1 us before IBF becomes visible after a write.
     * Polling immediately can observe the old clear bit and overwrite input. */
    transport->ops.delay_us(transport->ops.context, 1);
    return wait_input_empty(transport);
}

static int receive_byte(struct ec_transport *transport, uint8_t *value)
{
    int status = wait_status(transport, EC_STATUS_OBF, EC_STATUS_OBF, NULL);
    if (status)
        return status;
    if (transport->ops.read(transport->ops.context, EC_DATA_PORT, value))
        return EC_IO_ERROR;
    return EC_OK;
}

static int begin_transaction(struct ec_transport *transport)
{
    if (transport->quarantined)
        return EC_QUARANTINED;
    uint8_t status;
    int result = wait_status(transport, EC_STATUS_IBF, 0, &status);
    if (result)
        return result;
    /* An unexpected output byte might belong to an unfinished transaction or
     * another owner. Discarding it would manufacture a false command boundary. */
    if (status & EC_STATUS_OBF)
        return EC_PROTOCOL;
    return EC_OK;
}

static int finish_transaction(struct ec_transport *transport, int status)
{
    if (status == EC_OK) {
        transport->completed++;
    } else {
        transport->last_error = status;
        /* IBF=0/OBF=0 alone cannot prove the EC's command parser is idle: it
         * might still be waiting for an address/data byte. Without a reset
         * ownership protocol, recovery means isolating this binding until
         * reboot, not blindly replaying a possibly completed write. */
        transport->quarantined = 1;
    }
    transport->ops.unlock(transport->ops.context);
    return status;
}

static int valid_transfer(struct ec_transport *transport, unsigned address,
                          unsigned width)
{
    return transport && width && width <= EC_MAX_TRANSFER_BYTES &&
           address < EC_ADDRESS_BYTES && width <= EC_ADDRESS_BYTES - address;
}

int ec_transport_read(struct ec_transport *transport, unsigned address, unsigned width,
                      uint64_t *value)
{
    if (!value || !valid_transfer(transport, address, width))
        return EC_INVALID;
    if (transport->ops.lock(transport->ops.context))
        return EC_IO_ERROR;

    int status = begin_transaction(transport);
    uint64_t result = 0;
    for (unsigned index = 0; !status && index < width; index++) {
        uint8_t byte;
        status = send_byte(transport, EC_CONTROL_PORT, EC_COMMAND_READ);
        if (!status)
            status = send_byte(transport, EC_DATA_PORT, (uint8_t)(address + index));
        if (!status)
            status = receive_byte(transport, &byte);
        if (!status)
            result |= (uint64_t)byte << (index * 8);
    }
    if (!status)
        *value = result;
    return finish_transaction(transport, status);
}

int ec_transport_write(struct ec_transport *transport, unsigned address, unsigned width,
                       uint64_t value)
{
    if (!valid_transfer(transport, address, width))
        return EC_INVALID;
    if (transport->ops.lock(transport->ops.context))
        return EC_IO_ERROR;

    int status = begin_transaction(transport);
    for (unsigned index = 0; !status && index < width; index++) {
        status = send_byte(transport, EC_CONTROL_PORT, EC_COMMAND_WRITE);
        if (!status)
            status = send_byte(transport, EC_DATA_PORT, (uint8_t)(address + index));
        if (!status)
            status =
                send_byte(transport, EC_DATA_PORT, (uint8_t)(value >> (index * 8)));
    }
    return finish_transaction(transport, status);
}

int ec_transport_query(struct ec_transport *transport, uint8_t *query)
{
    if (!transport || !query)
        return EC_INVALID;
    if (transport->ops.lock(transport->ops.context))
        return EC_IO_ERROR;

    int status = begin_transaction(transport);
    uint8_t flags = 0;
    uint8_t result = 0;
    if (!status && transport->ops.read(transport->ops.context, EC_CONTROL_PORT, &flags))
        status = EC_IO_ERROR;
    if (!status && (flags & EC_STATUS_SCI_EVT)) {
        status = send_byte(transport, EC_CONTROL_PORT, EC_COMMAND_QUERY);
        if (!status)
            status = receive_byte(transport, &result);
    }
    if (!status)
        *query = result;
    return finish_transaction(transport, status);
}
