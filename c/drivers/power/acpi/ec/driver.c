#include "driver.h"
#include "resources.h"
#include "transport.h"
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define EC_MAX_CONTROLLERS 4
#define EC_HANDSHAKE_TIMEOUT_US 250000
#define EC_QUERY_BUDGET 32
#define EC_STATUS_SCI_EVT (1u << 5)

struct ec_binding {
    uacpi_namespace_node *node;
    struct ec_resources resources;
    struct ec_transport transport;
    uacpi_handle ports[2];
    uacpi_handle mutex;
    unsigned query_queued;
    int accepting;
    int active;
    int gpe_installed;
    int region_installed;
    int last_error;
};
static struct ec_binding controllers[EC_MAX_CONTROLLERS];
static unsigned controller_slots;
static int discovery_error;
static int discovery_attempted;

static int lock_transport(void *context)
{
    struct ec_binding *binding = context;
    return uacpi_kernel_acquire_mutex(binding->mutex, UINT16_MAX) != UACPI_STATUS_OK;
}

static void unlock_transport(void *context)
{
    struct ec_binding *binding = context;
    uacpi_kernel_release_mutex(binding->mutex);
}

static int read_port(void *context, enum ec_port port, uint8_t *value)
{
    struct ec_binding *binding = context;
    return uacpi_kernel_io_read8(binding->ports[port], 0, value) != UACPI_STATUS_OK;
}

static int write_port(void *context, enum ec_port port, uint8_t value)
{
    struct ec_binding *binding = context;
    return uacpi_kernel_io_write8(binding->ports[port], 0, value) != UACPI_STATUS_OK;
}

static uint64_t now_us(void *context)
{
    (void)context;
    return uacpi_kernel_get_nanoseconds_since_boot() / 1000;
}

static void delay_us(void *context, unsigned microseconds)
{
    (void)context;
    /* The transport's delays are 1 or 10 us, within uACPI's u8 stall ABI. */
    uacpi_kernel_stall((uacpi_u8)microseconds);
}

static uacpi_status transport_status(int result)
{
    switch (result) {
    case EC_OK:
        return UACPI_STATUS_OK;
    case EC_INVALID:
        return UACPI_STATUS_INVALID_ARGUMENT;
    case EC_TIMEOUT:
        return UACPI_STATUS_HARDWARE_TIMEOUT;
    case EC_QUARANTINED:
        return UACPI_STATUS_DENIED;
    default:
        return UACPI_STATUS_INTERNAL_ERROR;
    }
}

static uacpi_status access_region(uacpi_region_op operation, uacpi_handle data)
{
    if (operation == UACPI_REGION_OP_ATTACH) {
        uacpi_region_attach_data *attach = data;
        struct ec_binding *binding = attach->handler_context;
        if (!__atomic_load_n(&binding->accepting, __ATOMIC_ACQUIRE))
            return UACPI_STATUS_NO_HANDLER;
        if (!attach->generic_info.length || attach->generic_info.base >= 256 ||
            attach->generic_info.length > 256 - attach->generic_info.base)
            return UACPI_STATUS_AML_OUT_OF_BOUNDS_INDEX;
        /* The binding is boot-lifetime storage. Region attachment borrows it;
         * no per-region allocation or independent port ownership is created. */
        attach->out_region_context = binding;
        return UACPI_STATUS_OK;
    }
    if (operation == UACPI_REGION_OP_DETACH)
        return UACPI_STATUS_OK;
    if (operation != UACPI_REGION_OP_READ && operation != UACPI_REGION_OP_WRITE)
        return UACPI_STATUS_UNIMPLEMENTED;

    uacpi_region_rw_data *request = data;
    struct ec_binding *binding = request->region_context;
    if (!binding || !__atomic_load_n(&binding->accepting, __ATOMIC_ACQUIRE))
        return UACPI_STATUS_NO_HANDLER;
    if (request->address >= 256)
        return UACPI_STATUS_AML_OUT_OF_BOUNDS_INDEX;

    int result;
    if (operation == UACPI_REGION_OP_READ) {
        uint64_t value;
        result = ec_transport_read(&binding->transport, (unsigned)request->address,
                                   request->byte_width, &value);
        if (result == EC_OK)
            request->value = value;
    } else {
        result = ec_transport_write(&binding->transport, (unsigned)request->address,
                                    request->byte_width, request->value);
    }
    if (result)
        __atomic_store_n(&binding->last_error, (int)transport_status(result),
                         __ATOMIC_RELEASE);
    return transport_status(result);
}

static void run_queries(uacpi_handle context)
{
    struct ec_binding *binding = context;
    uacpi_status status = UACPI_STATUS_OK;
    unsigned processed = 0;
    for (; processed < EC_QUERY_BUDGET; processed++) {
        uint8_t query;
        status = transport_status(ec_transport_query(&binding->transport, &query));
        if (status != UACPI_STATUS_OK || !query)
            break;

        static const char hex[] = "0123456789ABCDEF";
        char method[] = "_Q00";
        method[2] = hex[query >> 4];
        method[3] = hex[query & 15];
        /* Release the transport mutex before AML: a _Qxx method may itself
         * access an EC Field. Holding it across eval would self-deadlock. */
        status = uacpi_eval(binding->node, method, NULL, NULL);
        if (status != UACPI_STATUS_OK)
            break;
    }
    if (processed == EC_QUERY_BUDGET)
        status = UACPI_STATUS_TIMEOUT;
    if (status != UACPI_STATUS_OK) {
        __atomic_store_n(&binding->last_error, (int)status, __ATOMIC_RELEASE);
        /* A query storm or broken transaction stays masked. Do not re-enable
         * an edge source after losing ownership of its pending notification. */
        uacpi_mask_gpe(NULL, binding->resources.gpe);
    }
    __atomic_store_n(&binding->query_queued, 0, __ATOMIC_RELEASE);
    if (status == UACPI_STATUS_OK) {
        status = uacpi_finish_handling_gpe(NULL, binding->resources.gpe);
        if (status != UACPI_STATUS_OK)
            __atomic_store_n(&binding->last_error, (int)status, __ATOMIC_RELEASE);
    }
}

static uacpi_interrupt_ret
handle_ec_gpe(uacpi_handle context, uacpi_namespace_node *gpe_device, uacpi_u16 index)
{
    (void)gpe_device;
    (void)index;
    struct ec_binding *binding = context;
    uint8_t status;
    if (!__atomic_load_n(&binding->accepting, __ATOMIC_ACQUIRE))
        return UACPI_INTERRUPT_HANDLED;
    if (read_port(binding, EC_CONTROL_PORT, &status)) {
        __atomic_store_n(&binding->last_error, UACPI_STATUS_INTERNAL_ERROR,
                         __ATOMIC_RELEASE);
        return UACPI_INTERRUPT_HANDLED;
    }
    if (!(status & EC_STATUS_SCI_EVT))
        return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;

    unsigned expected = 0;
    if (__atomic_compare_exchange_n(&binding->query_queued, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        uacpi_status queued =
            uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, run_queries, binding);
        if (queued != UACPI_STATUS_OK) {
            __atomic_store_n(&binding->query_queued, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&binding->last_error, (int)queued, __ATOMIC_RELEASE);
        }
    }
    /* uACPI disabled this non-raw GPE before invoking us. The worker reenables
     * it after consuming queries; no sleeping or command exchange occurs here. */
    return UACPI_INTERRUPT_HANDLED;
}

static void close_unpublished_binding(struct ec_binding *binding)
{
    for (unsigned index = 0; index < 2; index++) {
        if (binding->ports[index]) {
            uacpi_kernel_io_unmap(binding->ports[index]);
            binding->ports[index] = NULL;
        }
    }
    if (binding->mutex) {
        uacpi_kernel_free_mutex(binding->mutex);
        binding->mutex = NULL;
    }
}

static uacpi_status bind_controller(uacpi_namespace_node *node)
{
    struct ec_resources resources;
    uacpi_status status = ec_resources_from_device(node, &resources);
    if (status != UACPI_STATUS_OK)
        return status;

    for (unsigned index = 0; index < controller_slots; index++) {
        const struct ec_resources *claimed = &controllers[index].resources;
        if (claimed->data_port == resources.data_port ||
            claimed->data_port == resources.control_port ||
            claimed->control_port == resources.data_port ||
            claimed->control_port == resources.control_port ||
            claimed->gpe == resources.gpe)
            return UACPI_STATUS_ALREADY_EXISTS;
    }
    if (controller_slots == EC_MAX_CONTROLLERS)
        return UACPI_STATUS_OUT_OF_MEMORY;

    uacpi_event_info gpe_info;
    status = uacpi_gpe_info(NULL, resources.gpe, &gpe_info);
    if (status != UACPI_STATUS_OK)
        return status;
    unsigned occupied = UACPI_EVENT_INFO_HAS_HANDLER | UACPI_EVENT_INFO_ENABLED |
                        UACPI_EVENT_INFO_ENABLED_FOR_WAKE | UACPI_EVENT_INFO_MASKED |
                        UACPI_EVENT_INFO_HW_ENABLED;
    if (gpe_info & occupied)
        return UACPI_STATUS_ALREADY_EXISTS;

    struct ec_binding *binding = &controllers[controller_slots++];
    binding->node = node;
    binding->resources = resources;
    binding->mutex = uacpi_kernel_create_mutex();
    if (!binding->mutex)
        return UACPI_STATUS_OUT_OF_MEMORY;
    status = uacpi_kernel_io_map(resources.data_port, 1, &binding->ports[EC_DATA_PORT]);
    if (status == UACPI_STATUS_OK)
        status = uacpi_kernel_io_map(resources.control_port, 1,
                                     &binding->ports[EC_CONTROL_PORT]);
    if (status != UACPI_STATUS_OK) {
        close_unpublished_binding(binding);
        return status;
    }

    struct ec_transport_ops ops = {.context = binding,
                                   .lock = lock_transport,
                                   .unlock = unlock_transport,
                                   .read = read_port,
                                   .write = write_port,
                                   .now_us = now_us,
                                   .delay_us = delay_us};
    if (ec_transport_init(&binding->transport, &ops, EC_HANDSHAKE_TIMEOUT_US)) {
        close_unpublished_binding(binding);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    status = uacpi_install_gpe_handler(NULL, resources.gpe, UACPI_GPE_TRIGGERING_EDGE,
                                       handle_ec_gpe, binding);
    if (status != UACPI_STATUS_OK) {
        close_unpublished_binding(binding);
        return status;
    }
    binding->gpe_installed = 1;
    __atomic_store_n(&binding->accepting, 1, __ATOMIC_RELEASE);

    /* At NAMESPACE_LOADED the uACPI install routine connects OperationRegions
     * and executes _REG(EmbeddedControl,1). Do this before _INI, which often
     * reads EC fields, and before enabling asynchronous GPE query dispatch. */
    status = uacpi_install_address_space_handler(
        node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, access_region, binding);
    if (status == UACPI_STATUS_OK)
        binding->region_installed = 1;
    if (status != UACPI_STATUS_OK) {
        __atomic_store_n(&binding->accepting, 0, __ATOMIC_RELEASE);
        binding->last_error = status;
        uacpi_mask_gpe(NULL, resources.gpe);
        /* Handler installation can partially publish before _REG reports an
         * error. Keep this bounded slot and its resources alive rather than
         * free context still referenced by an attached OperationRegion. */
        return status;
    }
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision
visit_controller(void *context, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)context;
    (void)depth;
    uacpi_status status = bind_controller(node);
    if (status != UACPI_STATUS_OK) {
        if (!discovery_error)
            discovery_error = (int)status;
        uacpi_kernel_log(UACPI_LOG_WARN,
                         "EC binding rejected; inspect power EC status\n");
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

uacpi_status power_ec_initialize(void)
{
    if (discovery_attempted)
        return (uacpi_status)discovery_error;
    discovery_attempted = 1;
    uacpi_status status = uacpi_find_devices("PNP0C09", visit_controller, NULL);
    if (status != UACPI_STATUS_OK && !discovery_error)
        discovery_error = (int)status;
    return (uacpi_status)discovery_error;
}

uacpi_status power_ec_activate(void)
{
    int first_error = discovery_error;
    for (unsigned index = 0; index < controller_slots; index++) {
        struct ec_binding *binding = &controllers[index];
        if (binding->active || !binding->region_installed ||
            !__atomic_load_n(&binding->accepting, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&binding->last_error, __ATOMIC_ACQUIRE))
            continue;

        /* _REG and _INI use the polled transport while GPE delivery remains
         * disabled. Only after namespace initialization may _Qxx execute. */
        uacpi_status status = uacpi_enable_gpe(NULL, binding->resources.gpe);
        if (status == UACPI_STATUS_OK) {
            binding->active = 1;
            uint8_t flags;
            if (read_port(binding, EC_CONTROL_PORT, &flags)) {
                status = UACPI_STATUS_INTERNAL_ERROR;
            } else if (flags & EC_STATUS_SCI_EVT) {
                /* Namespace loading cleared GPE status, but an EC notification
                 * may still be latched. Suspend before scheduling the query so
                 * the same masked-until-complete rule applies as in the ISR. */
                status = uacpi_suspend_gpe(NULL, binding->resources.gpe);
                if (status == UACPI_STATUS_OK) {
                    uacpi_interrupt_ret result =
                        handle_ec_gpe(binding, NULL, binding->resources.gpe);
                    if (result & UACPI_GPE_REENABLE)
                        status = uacpi_resume_gpe(NULL, binding->resources.gpe);
                }
            }
        }
        if (status != UACPI_STATUS_OK) {
            __atomic_store_n(&binding->last_error, (int)status, __ATOMIC_RELEASE);
            uacpi_mask_gpe(NULL, binding->resources.gpe);
            if (!first_error)
                first_error = (int)status;
        }
    }
    return (uacpi_status)first_error;
}

void power_ec_status(unsigned *bound, int *error)
{
    unsigned count = 0;
    int result = discovery_error;
    for (unsigned index = 0; index < controller_slots; index++) {
        struct ec_binding *binding = &controllers[index];
        if (binding->active)
            count++;
        int binding_error = __atomic_load_n(&binding->last_error, __ATOMIC_ACQUIRE);
        if (!result)
            result = binding_error;
    }
    if (bound)
        *bound = count;
    if (error)
        *error = result;
}
