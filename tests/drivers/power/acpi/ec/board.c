/* Deterministic full-ACPI board: PM1, GPE0 and an EC at deliberately non-default
 * ports. The production resource resolver, transport, region handler and uACPI
 * SCI/GPE dispatcher all run; only the physical register bus is modeled. */
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uacpi/kernel_api.h>

#define DATA_PORT 0x260u
#define CONTROL_PORT 0x264u
#define PM1_STATUS 0x4000u
#define PM1_ENABLE 0x4002u
#define PM1_CONTROL 0x4004u
#define GPE_STATUS 0x4010u
#define GPE_ENABLE 0x4011u
#define EC_GPE_MASK 8u

struct ec_model power_ec_model;
static uint8_t io_space[65536];
static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_context;
struct host_job {
    uacpi_work_handler handler;
    uacpi_handle context;
};
static struct host_job jobs[64];
static unsigned job_count;
static unsigned early_gpe_enable;

void power_ec_board_init(const char *scenario)
{
    ec_model_init(&power_ec_model);
    memset(io_space, 0, sizeof io_space);
    io_space[PM1_CONTROL] = 1; /* SCI_EN: firmware is already in ACPI mode. */
    power_ec_model.memory[0x20] = 0xcc;
    power_ec_model.memory[0x21] = 0x10; /* 4300 capacity units */
    power_ec_model.memory[0x30] = 0xb9;
    power_ec_model.memory[0x31] = 0x0b; /* 3001 tenths kelvin */
    if (!strcmp(scenario, "timeout"))
        power_ec_model.missing_output = 1;
    if (!strcmp(scenario, "pending-query")) {
        power_ec_model.event_pending = 1;
        power_ec_model.query_code = 0x42;
    }
}

struct io_range {
    unsigned base;
    size_t length;
};
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length,
                                 uacpi_handle *out)
{
    if (!out || !length || base >= sizeof io_space || length > sizeof io_space - base)
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct io_range *range = malloc(sizeof *range);
    if (!range)
        return UACPI_STATUS_OUT_OF_MEMORY;
    range->base = (unsigned)base;
    range->length = length;
    *out = range;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    free(handle);
}

static uacpi_status io_read(uacpi_handle handle, size_t offset, void *out, size_t width)
{
    struct io_range *range = handle;
    if (!range || !out || offset > range->length || width > range->length - offset)
        return UACPI_STATUS_INVALID_ARGUMENT;
    unsigned port = range->base + (unsigned)offset;
    if (port == DATA_PORT || port == CONTROL_PORT) {
        if (width != 1)
            return UACPI_STATUS_INVALID_ARGUMENT;
        return ec_model_read(&power_ec_model,
                             port == DATA_PORT ? EC_DATA_PORT : EC_CONTROL_PORT, out)
                   ? UACPI_STATUS_INTERNAL_ERROR
                   : UACPI_STATUS_OK;
    }
    memcpy(out, io_space + port, width);
    return UACPI_STATUS_OK;
}

static uacpi_status io_write(uacpi_handle handle, size_t offset, const void *in,
                             size_t width)
{
    struct io_range *range = handle;
    if (!range || offset > range->length || width > range->length - offset)
        return UACPI_STATUS_INVALID_ARGUMENT;
    unsigned port = range->base + (unsigned)offset;
    if (port == DATA_PORT || port == CONTROL_PORT) {
        if (width != 1)
            return UACPI_STATUS_INVALID_ARGUMENT;
        return ec_model_write(&power_ec_model,
                              port == DATA_PORT ? EC_DATA_PORT : EC_CONTROL_PORT,
                              *(const uint8_t *)in)
                   ? UACPI_STATUS_INTERNAL_ERROR
                   : UACPI_STATUS_OK;
    }
    if (port == GPE_ENABLE && (*(const uint8_t *)in & EC_GPE_MASK) &&
        power_ec_model.memory[0x11] != 1)
        early_gpe_enable++;
    /* PM status and GPE status are W1C; enable/control registers are ordinary
     * storage. Treating all writes as memcpy would fabricate interrupt storms. */
    if (port == PM1_STATUS || port == GPE_STATUS) {
        for (size_t index = 0; index < width; index++)
            io_space[port + index] &= ~((const uint8_t *)in)[index];
    } else {
        memcpy(io_space + port, in, width);
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
                                                    uacpi_interrupt_handler handler,
                                                    uacpi_handle context,
                                                    uacpi_handle *out)
{
    if (irq != 9 || !handler || !out || sci_handler)
        return UACPI_STATUS_INVALID_ARGUMENT;
    sci_handler = handler;
    sci_context = context;
    *out = &sci_handler;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                                      uacpi_handle handle)
{
    if (handler != sci_handler || handle != &sci_handler)
        return UACPI_STATUS_INVALID_ARGUMENT;
    sci_handler = NULL;
    sci_context = NULL;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler handler,
                                        uacpi_handle context)
{
    if (!handler ||
        (type != UACPI_WORK_GPE_EXECUTION && type != UACPI_WORK_NOTIFICATION))
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (job_count == 64)
        return UACPI_STATUS_OUT_OF_MEMORY;
    jobs[job_count++] = (struct host_job){handler, context};
    return UACPI_STATUS_OK;
}

void power_ec_board_drain(void)
{
    unsigned budget = 128;
    while (job_count && budget--) {
        struct host_job job = jobs[0];
        memmove(jobs, jobs + 1, --job_count * sizeof jobs[0]);
        job.handler(job.context);
    }
    if (job_count) {
        fputs("test board work queue did not drain\n", stderr);
        abort();
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    power_ec_board_drain();
    return UACPI_STATUS_OK;
}

void power_ec_board_raise_query(uint8_t query)
{
    power_ec_model.event_pending = 1;
    power_ec_model.query_code = query;
    io_space[GPE_STATUS] |= EC_GPE_MASK;
    if (sci_handler && (io_space[GPE_ENABLE] & EC_GPE_MASK))
        sci_handler(sci_context);
}

unsigned power_ec_board_gpe_enabled(void)
{
    return !!(io_space[GPE_ENABLE] & EC_GPE_MASK);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset,
                                   uacpi_u8 *value)
{
    return io_read(handle, offset, value, sizeof *value);
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 value)
{
    return io_write(handle, offset, &value, sizeof value);
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u16 *value)
{
    return io_read(handle, offset, value, sizeof *value);
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 value)
{
    return io_write(handle, offset, &value, sizeof value);
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u32 *value)
{
    return io_read(handle, offset, value, sizeof *value);
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 value)
{
    return io_write(handle, offset, &value, sizeof value);
}

unsigned power_ec_board_early_gpe_enable(void)
{
    return early_gpe_enable;
}
