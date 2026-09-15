/* The oracle uses literal hardware commands/status bits, independently of the
 * production enums. Writes become IBF-visible after 1 us and are consumed after
 * 30 us; a model that cleared IBF synchronously could not catch early writes. */
#include "model.h"
#include <string.h>

enum { IDLE, READ_ADDRESS, WRITE_ADDRESS, WRITE_VALUE };

void ec_model_init(struct ec_model *model)
{
    memset(model, 0, sizeof *model);
}

static void consume_input(struct ec_model *model)
{
    if (!model->pending_input || model->now < model->input_consumed_at ||
        model->stuck_input)
        return;
    uint8_t value = model->pending_value;
    model->pending_input = 0;
    if (model->pending_port == EC_CONTROL_PORT) {
        if (model->phase != IDLE) {
            model->early_writes++;
            return;
        }
        if (value == 0x80) {
            model->command_reads++;
            model->phase = READ_ADDRESS;
        } else if (value == 0x81) {
            model->command_writes++;
            model->phase = WRITE_ADDRESS;
        } else if (value == 0x84) {
            model->queries++;
            model->output = model->query_code;
            model->output_is_query = 1;
            model->output_pending = 1;
            model->output_ready_at = model->now + 20;
        } else {
            model->early_writes++;
        }
    } else if (model->phase == READ_ADDRESS) {
        model->output = model->memory[value];
        model->output_is_query = 0;
        model->output_pending = 1;
        model->output_ready_at = model->now + 20;
        model->phase = IDLE;
    } else if (model->phase == WRITE_ADDRESS) {
        model->address = value;
        model->phase = WRITE_VALUE;
    } else if (model->phase == WRITE_VALUE) {
        model->memory[model->address] = value;
        model->phase = IDLE;
    } else {
        model->early_writes++;
    }
}

int ec_model_read(void *context, enum ec_port port, uint8_t *value)
{
    struct ec_model *model = context;
    consume_input(model);
    int output_full = model->stale_output ||
                      (model->output_pending && model->now >= model->output_ready_at &&
                       !model->missing_output);
    if (port == EC_CONTROL_PORT) {
        *value =
            (model->pending_input && model->now >= model->input_visible_at ? 0x02 : 0) |
            (model->stuck_input ? 0x02 : 0) | (output_full ? 0x01 : 0) |
            (model->event_pending ? 0x20 : 0) | (model->shared_status ? 0x40 : 0);
        return 0;
    }
    if (!output_full) {
        model->invalid_reads++;
        return -1;
    }
    *value = model->output;
    model->output_pending = 0;
    model->stale_output = 0;
    if (model->event_pending && model->output_is_query)
        model->event_pending = 0;
    return 0;
}

int ec_model_write(void *context, enum ec_port port, uint8_t value)
{
    struct ec_model *model = context;
    consume_input(model);
    model->port_writes++;
    if (model->fault_at_write && model->port_writes == model->fault_at_write)
        return -1;
    if (model->pending_input) {
        model->early_writes++;
        return -1;
    }
    model->pending_input = 1;
    model->pending_port = port;
    model->pending_value = value;
    model->input_visible_at = model->now + 1;
    model->input_consumed_at = model->now + 30;
    return 0;
}

uint64_t ec_model_now(void *context)
{
    return ((struct ec_model *)context)->now;
}

void ec_model_delay(void *context, unsigned microseconds)
{
    ((struct ec_model *)context)->now += microseconds;
}

static int lock_model(void *context)
{
    struct ec_model *model = context;
    if (model->lock_depth)
        return -1;
    model->lock_depth++;
    return 0;
}

static void unlock_model(void *context)
{
    ((struct ec_model *)context)->lock_depth--;
}

struct ec_transport_ops ec_model_ops(struct ec_model *model)
{
    return (struct ec_transport_ops){.context = model,
                                     .lock = lock_model,
                                     .unlock = unlock_model,
                                     .read = ec_model_read,
                                     .write = ec_model_write,
                                     .now_us = ec_model_now,
                                     .delay_us = ec_model_delay};
}
