#include "resources.h"
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

enum { EC_PORT_COUNT = 2, EC_ECDT_PATH_MAX = 256 };
struct resource_walk {
    uint16_t ports[EC_PORT_COUNT];
    unsigned count;
    uacpi_status error;
};

static uacpi_iteration_decision visit_resource(void *context, uacpi_resource *resource)
{
    struct resource_walk *walk = context;
    if (resource->type == UACPI_RESOURCE_TYPE_END_TAG)
        return UACPI_ITERATION_DECISION_CONTINUE;

    /* FixedIO and 10-bit IO descriptors alias other port ranges. This driver
     * has no allocator capable of reserving every alias, so it requires the
     * unambiguous 16-bit IO form rather than assuming ownership of a pair. */
    if (resource->type != UACPI_RESOURCE_TYPE_IO ||
        resource->io.decode_type != UACPI_DECODE_16 ||
        resource->io.minimum != resource->io.maximum || resource->io.length != 1 ||
        !resource->io.minimum || walk->count == EC_PORT_COUNT) {
        walk->error = UACPI_STATUS_UNIMPLEMENTED;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    walk->ports[walk->count++] = resource->io.minimum;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static int path_is_terminated(const char *path, size_t capacity)
{
    for (size_t index = 0; index < capacity; index++) {
        if (!path[index])
            return 1;
    }
    return 0;
}

static int gas_is_byte_io(const struct acpi_gas *gas)
{
    return gas->address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO &&
           gas->register_bit_width == 8 && gas->register_bit_offset == 0 &&
           gas->access_size <= 1 && gas->address && gas->address <= UINT16_MAX;
}

static uacpi_status check_ecdt(uacpi_namespace_node *node,
                               const struct ec_resources *resources)
{
    uacpi_table table;
    uacpi_status status = uacpi_table_find_by_signature("ECDT", &table);
    if (status == UACPI_STATUS_NOT_FOUND)
        return UACPI_STATUS_OK;
    if (status != UACPI_STATUS_OK)
        return status;

    const struct acpi_ecdt *ecdt = table.ptr;
    if (table.hdr->length <= sizeof *ecdt ||
        table.hdr->length - sizeof *ecdt > EC_ECDT_PATH_MAX) {
        status = UACPI_STATUS_INVALID_TABLE_LENGTH;
        goto release;
    }
    size_t path_capacity = table.hdr->length - sizeof *ecdt;
    if (ecdt->ec_id[0] != '\\' || !path_is_terminated(ecdt->ec_id, path_capacity)) {
        status = UACPI_STATUS_AML_BAD_ENCODING;
        goto release;
    }

    uacpi_namespace_node *ecdt_node;
    status = uacpi_namespace_node_find(uacpi_namespace_root(), ecdt->ec_id, &ecdt_node);
    if (status != UACPI_STATUS_OK || ecdt_node != node)
        goto release;
    if (!gas_is_byte_io(&ecdt->ec_control) || !gas_is_byte_io(&ecdt->ec_data) ||
        ecdt->ec_control.address != resources->control_port ||
        ecdt->ec_data.address != resources->data_port ||
        ecdt->gpe_bit != resources->gpe) {
        status = UACPI_STATUS_INVALID_ARGUMENT;
        goto release;
    }

    uacpi_u64 uid;
    status = uacpi_eval_simple_integer(node, "_UID", &uid);
    if (status == UACPI_STATUS_NOT_FOUND)
        status = UACPI_STATUS_OK;
    else if (status == UACPI_STATUS_OK && uid != ecdt->uid)
        status = UACPI_STATUS_INVALID_ARGUMENT;
release:
    uacpi_table_unref(&table);
    return status;
}

uacpi_status ec_resources_from_device(uacpi_namespace_node *node,
                                      struct ec_resources *out)
{
    if (!node || !out)
        return UACPI_STATUS_INVALID_ARGUMENT;

    uacpi_u32 device_status;
    uacpi_status status = uacpi_eval_sta(node, &device_status);
    unsigned required = ACPI_STA_RESULT_DEVICE_PRESENT |
                        ACPI_STA_RESULT_DEVICE_ENABLED |
                        ACPI_STA_RESULT_DEVICE_FUNCTIONING;
    if (status != UACPI_STATUS_OK)
        return status;
    if ((device_status & required) != required)
        return UACPI_STATUS_NOT_FOUND;

    uacpi_u64 shared = 0;
    status = uacpi_eval_simple_integer(node, "_GLK", &shared);
    if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND)
        return status;
    if (shared)
        return UACPI_STATUS_UNIMPLEMENTED;

    uacpi_u64 gpe;
    status = uacpi_eval_simple_integer(node, "_GPE", &gpe);
    if (status != UACPI_STATUS_OK)
        return status;
    if (gpe > UINT16_MAX)
        return UACPI_STATUS_INVALID_ARGUMENT;

    struct resource_walk walk = {0};
    status = uacpi_for_each_device_resource(node, "_CRS", visit_resource, &walk);
    if (status != UACPI_STATUS_OK)
        return status;
    if (walk.error)
        return walk.error;
    if (walk.count != EC_PORT_COUNT || walk.ports[0] == walk.ports[1])
        return UACPI_STATUS_INVALID_ARGUMENT;

    /* ACPI 12.11 orders EC_DATA before EC_SC in _CRS. Swapping this order
     * writes command bytes into the data register and corrupts the protocol. */
    struct ec_resources result = {.data_port = walk.ports[0],
                                  .control_port = walk.ports[1],
                                  .gpe = (uint16_t)gpe};
    status = check_ecdt(node, &result);
    if (status == UACPI_STATUS_OK)
        *out = result;
    return status;
}
