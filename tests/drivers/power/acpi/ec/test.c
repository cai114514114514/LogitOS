#include "model.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(expression)                                                              \
    do {                                                                               \
        checks++;                                                                      \
        if (!(expression)) {                                                           \
            fprintf(stderr, "FAIL: %s\n", #expression);                                \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void fresh(struct ec_transport *transport, struct ec_model *model)
{
    ec_model_init(model);
    struct ec_transport_ops ops = ec_model_ops(model);
    CHECK(ec_transport_init(transport, &ops, 1000) == EC_OK);
}

static uint64_t stopped_clock(void *context)
{
    (void)context;
    return 0;
}

static void stopped_delay(void *context, unsigned microseconds)
{
    (void)context;
    (void)microseconds;
}

int main(void)
{
    struct ec_transport transport;
    struct ec_model model;
    uint64_t value = 0;
    fresh(&transport, &model);
    for (unsigned index = 0; index < 256; index++)
        model.memory[index] = (uint8_t)(index ^ 0x5a);
    CHECK(ec_transport_read(&transport, 0x20, 2, &value) == EC_OK);
    CHECK(value == 0x7b7a);
    CHECK(model.command_reads == 2);
    CHECK(model.early_writes == 0);
    CHECK(model.invalid_reads == 0);
    CHECK(model.lock_depth == 0);
    CHECK(ec_transport_write(&transport, 0x30, 2, 0x1234) == EC_OK);
    CHECK(model.memory[0x30] == 0x34 && model.memory[0x31] == 0x12);
    CHECK(model.command_writes == 2);
    CHECK(ec_transport_read(&transport, 0, 8, &value) == EC_OK);
    CHECK(value == 0x5d5c5f5e59585b5aull);
    CHECK(ec_transport_write(&transport, 0, 8, 0x8877665544332211ull) == EC_OK);
    CHECK(model.memory[0] == 0x11 && model.memory[7] == 0x88);
    CHECK(ec_transport_read(&transport, 0xff, 2, &value) == EC_INVALID);
    CHECK(ec_transport_read(&transport, 0, 0, &value) == EC_INVALID);
    CHECK(ec_transport_read(&transport, 0, 9, &value) == EC_INVALID);
    CHECK(ec_transport_read(&transport, 0, 1, NULL) == EC_INVALID);
    CHECK(model.lock_depth == 0);

    fresh(&transport, &model);
    uint8_t query = 0xee;
    CHECK(ec_transport_query(&transport, &query) == EC_OK);
    CHECK(query == 0 && model.queries == 0 && model.port_writes == 0);
    model.event_pending = 1;
    model.query_code = 0x42;
    CHECK(ec_transport_query(&transport, &query) == EC_OK);
    CHECK(query == 0x42 && model.queries == 1 && model.event_pending == 0);
    model.event_pending = 1;
    model.query_code = 0;
    CHECK(ec_transport_query(&transport, &query) == EC_OK);
    CHECK(query == 0 && model.queries == 2);

    fresh(&transport, &model);
    model.stuck_input = 1;
    value = 0xdeadbeef;
    CHECK(ec_transport_read(&transport, 0, 1, &value) == EC_TIMEOUT);
    CHECK(value == 0xdeadbeef && model.port_writes == 0);
    CHECK(model.now >= 1000 && model.now <= 1020);
    model.stuck_input = 0;
    CHECK(ec_transport_write(&transport, 0, 1, 1) == EC_QUARANTINED);
    CHECK(model.port_writes == 0 && model.lock_depth == 0);

    fresh(&transport, &model);
    model.missing_output = 1;
    value = 0xdeadbeef;
    CHECK(ec_transport_read(&transport, 0, 1, &value) == EC_TIMEOUT);
    CHECK(value == 0xdeadbeef && model.invalid_reads == 0);
    CHECK(transport.quarantined == 1 && model.lock_depth == 0);

    fresh(&transport, &model);
    model.stale_output = 1;
    CHECK(ec_transport_write(&transport, 0, 1, 1) == EC_PROTOCOL);
    CHECK(model.port_writes == 0 && model.stale_output == 1);

    fresh(&transport, &model);
    model.fault_at_write = 5; /* First byte committed; second byte's address fails. */
    CHECK(ec_transport_write(&transport, 0x30, 2, 0x1234) == EC_IO_ERROR);
    CHECK(model.memory[0x30] == 0x34 && model.memory[0x31] == 0);
    CHECK(transport.quarantined && model.lock_depth == 0);
    unsigned writes = model.port_writes;
    CHECK(ec_transport_write(&transport, 0x30, 2, 0x1234) == EC_QUARANTINED);
    CHECK(model.port_writes == writes);

    fresh(&transport, &model);
    model.event_pending = 1;
    model.missing_output = 1;
    query = 0xee;
    CHECK(ec_transport_query(&transport, &query) == EC_TIMEOUT);
    CHECK(query == 0xee && transport.quarantined);

    fresh(&transport, &model);
    model.stuck_input = 1;
    transport.ops.now_us = stopped_clock;
    transport.ops.delay_us = stopped_delay;
    CHECK(ec_transport_read(&transport, 0, 1, &value) == EC_TIMEOUT);
    CHECK(model.now == 0 && model.port_writes == 0 && model.lock_depth == 0);

    printf("EC_TRANSPORT: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
