#ifndef LOGIT_TEST_EC_MODEL_H
#define LOGIT_TEST_EC_MODEL_H
#include "transport.h"
#include <stdint.h>

struct ec_model {
    uint8_t memory[256];
    uint64_t now;
    uint64_t input_visible_at;
    uint64_t input_consumed_at;
    uint64_t output_ready_at;
    uint8_t pending_value;
    enum ec_port pending_port;
    int pending_input;
    int output_pending;
    int output_is_query;
    uint8_t output;
    unsigned phase;
    uint8_t address;
    unsigned lock_depth;
    unsigned early_writes;
    unsigned invalid_reads;
    unsigned command_reads;
    unsigned command_writes;
    unsigned queries;
    unsigned port_writes;
    unsigned fault_at_write;
    int stuck_input;
    int missing_output;
    int stale_output;
    int shared_status;
    int event_pending;
    uint8_t query_code;
};
void ec_model_init(struct ec_model *model);
int ec_model_read(void *context, enum ec_port port, uint8_t *value);
int ec_model_write(void *context, enum ec_port port, uint8_t value);
uint64_t ec_model_now(void *context);
void ec_model_delay(void *context, unsigned microseconds);
struct ec_transport_ops ec_model_ops(struct ec_model *model);
#endif
