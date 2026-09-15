#include "devices.h"
#include "ec/driver.h"

#include <string.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

enum {
    BATTERY_PRESENT = 1u << 4,
    BATTERY_DISCHARGING = 1u << 0,
    BATTERY_CHARGING = 1u << 1,
    BATTERY_STATE_MASK = 7,
    BATTERY_STATUS_FIELDS = 4,
    BATTERY_INFO_INTEGERS = 9,
    BATTERY_INFO_FIELDS = 13,
    BATTERY_UNIT_MWH = 0,
    BATTERY_UNIT_MAH = 1,
    MILLIDEGREES_PER_DECIKELVIN = 100,
    KELVIN_ZERO_MILLICELSIUS = 273150,
    MAX_CONVERTIBLE_DECIKELVIN = 21474836,
};

/* The consumer must serialize initialization and queries in sleepable thread
 * context. A failed initialization stays failed: retrying uACPI over a partly
 * initialized global namespace without its teardown would leak ownership. */
static int namespace_ready;
static int initialization_error;

static void copy_device_path(uacpi_namespace_node *node, char *destination,
                             size_t capacity)
{
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    if (!path) {
        destination[0] = '\0';
        return;
    }

    size_t length = 0;
    while (length + 1 < capacity && path[length]) {
        destination[length] = path[length];
        length++;
    }
    destination[length] = '\0';
    uacpi_free_absolute_path(path);
}

/* ACPI reports unsigned 32-bit values with 0xffffffff as unknown. Truncating
 * arbitrary 64-bit AML integers would turn malformed firmware into real data.
 * The caller supplies a temporary array; partly decoded results are never
 * published when a later package element fails validation. */
static int evaluate_integer_package(uacpi_namespace_node *node, const char *method,
                                    uint32_t *values, size_t integer_count,
                                    size_t element_count)
{
    uacpi_object *result = NULL;
    uacpi_object_array package;
    uacpi_status status = uacpi_eval(node, method, NULL, &result);

    if (status == UACPI_STATUS_OK)
        status = uacpi_object_get_package(result, &package);
    if (status == UACPI_STATUS_OK && package.count != element_count)
        status = UACPI_STATUS_AML_BAD_ENCODING;

    for (size_t index = 0; status == UACPI_STATUS_OK && index < integer_count;
         index++) {
        uacpi_u64 value;
        status = uacpi_object_get_integer(package.objects[index], &value);
        if (status == UACPI_STATUS_OK && value > UINT32_MAX)
            status = UACPI_STATUS_AML_BAD_ENCODING;
        if (status == UACPI_STATUS_OK)
            values[index] = (uint32_t)value;
    }

    /* uACPI owns every package element. Releasing the top-level object also
     * releases those elements, including on the malformed-package path. */
    uacpi_object_unref(result);
    return (int)status;
}

static void query_battery_status(uacpi_namespace_node *node,
                                 struct power_battery *battery)
{
    uint32_t values[BATTERY_STATUS_FIELDS];
    battery->status_error = evaluate_integer_package(
        node, "_BST", values, BATTERY_STATUS_FIELDS, BATTERY_STATUS_FIELDS);
    if (battery->status_error)
        return;

    uint32_t state = values[0];
    uint32_t charging_flags = BATTERY_CHARGING | BATTERY_DISCHARGING;
    if ((state & ~BATTERY_STATE_MASK) || (state & charging_flags) == charging_flags) {
        battery->status_error = UACPI_STATUS_AML_BAD_ENCODING;
        return;
    }

    battery->state = state;
    battery->rate = values[1];
    battery->remaining = values[2];
    battery->voltage = values[3];
}

static void query_battery_information(uacpi_namespace_node *node,
                                      struct power_battery *battery)
{
    uint32_t values[BATTERY_INFO_INTEGERS];
    battery->info_error = evaluate_integer_package(
        node, "_BIF", values, BATTERY_INFO_INTEGERS, BATTERY_INFO_FIELDS);
    if (battery->info_error)
        return;
    if (values[0] > BATTERY_UNIT_MAH) {
        battery->info_error = UACPI_STATUS_AML_BAD_ENCODING;
        return;
    }

    battery->unit = values[0];
    battery->design = values[1];
    battery->full = values[2];
    battery->design_voltage = values[4];
}

static uacpi_iteration_decision visit_battery(void *context, uacpi_namespace_node *node,
                                              uacpi_u32 depth)
{
    (void)depth;
    struct power_acpi_status *snapshot = context;
    if (snapshot->battery_count == POWER_BATTERIES) {
        snapshot->truncated = 1;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    struct power_battery *battery = &snapshot->batteries[snapshot->battery_count++];
    copy_device_path(node, battery->path, sizeof battery->path);
    battery->state = POWER_UNKNOWN;
    battery->rate = POWER_UNKNOWN;
    battery->remaining = POWER_UNKNOWN;
    battery->voltage = POWER_UNKNOWN;
    battery->unit = POWER_UNKNOWN;
    battery->design = POWER_UNKNOWN;
    battery->full = POWER_UNKNOWN;
    battery->design_voltage = POWER_UNKNOWN;

    uacpi_u32 device_status;
    battery->status_error = (int)uacpi_eval_sta(node, &device_status);
    if (battery->status_error)
        return UACPI_ITERATION_DECISION_CONTINUE;

    /* _STA bit 0 describes the ACPI device. Bit 4 separately reports whether
     * its battery is inserted; an empty battery bay must not be sampled. */
    battery->present = !!(device_status & BATTERY_PRESENT);
    if (!battery->present)
        return UACPI_ITERATION_DECISION_CONTINUE;

    query_battery_status(node, battery);
    query_battery_information(node, battery);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision
visit_ac_adapter(void *context, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    struct power_acpi_status *snapshot = context;
    uacpi_u64 online;
    uacpi_status status = uacpi_eval_simple_integer(node, "_PSR", &online);

    /* Any online adapter establishes AC power. Offline adapters cannot erase
     * that observation; failed methods leave the initial unknown value alone. */
    if (status == UACPI_STATUS_OK && online <= 1 && snapshot->ac_online != 1)
        snapshot->ac_online = (int)online;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static int evaluate_temperature(uacpi_namespace_node *node, const char *method,
                                int32_t *millicelsius)
{
    uacpi_u64 decikelvin;
    uacpi_status status = uacpi_eval_simple_integer(node, method, &decikelvin);

    /* _TMP/_CRT use tenths of kelvin, not Celsius. This bound rejects the
     * unknown sentinel as well as values that overflow the signed conversion. */
    if (status == UACPI_STATUS_OK && decikelvin > MAX_CONVERTIBLE_DECIKELVIN)
        status = UACPI_STATUS_AML_BAD_ENCODING;
    if (status == UACPI_STATUS_OK) {
        *millicelsius = (int32_t)(decikelvin * MILLIDEGREES_PER_DECIKELVIN) -
                        KELVIN_ZERO_MILLICELSIUS;
    }
    return (int)status;
}

static uacpi_iteration_decision
visit_thermal_zone(void *context, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    struct power_acpi_status *snapshot = context;
    if (snapshot->thermal_count == POWER_THERMALS) {
        snapshot->truncated = 1;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    struct power_thermal *thermal = &snapshot->thermals[snapshot->thermal_count++];
    copy_device_path(node, thermal->path, sizeof thermal->path);
    thermal->status_error =
        evaluate_temperature(node, "_TMP", &thermal->temperature_millic);
    thermal->critical_valid =
        !evaluate_temperature(node, "_CRT", &thermal->critical_millic);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

int power_acpi_start(void)
{
    if (namespace_ready)
        return 0;
    if (initialization_error)
        return initialization_error;

    uacpi_status status = uacpi_initialize(UACPI_FLAG_BAD_CSUM_FATAL |
                                           UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL);
    if (status == UACPI_STATUS_OK)
        status = uacpi_namespace_load();
    if (status == UACPI_STATUS_OK) {
        /* A rejected EC leaves its region unavailable, but must not hide
         * independent ACPI devices or disable a usable shutdown method. */
        power_ec_initialize();
        status = uacpi_namespace_initialize();
        if (status == UACPI_STATUS_OK)
            power_ec_activate();
    }

    initialization_error = (int)status;
    namespace_ready = status == UACPI_STATUS_OK;
    return initialization_error;
}

int power_acpi_query(struct power_acpi_status *out)
{
    if (!out)
        return -1;

    struct power_acpi_status snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.ready = namespace_ready;
    snapshot.error = initialization_error;
    snapshot.ac_online = -1;
    power_ec_status(&snapshot.ec_count, &snapshot.ec_error);
    if (!namespace_ready) {
        *out = snapshot;
        return -1;
    }

    uacpi_status battery_status =
        uacpi_find_devices("PNP0C0A", visit_battery, &snapshot);
    uacpi_status adapter_status =
        uacpi_find_devices("ACPI0003", visit_ac_adapter, &snapshot);
    uacpi_status thermal_status = uacpi_namespace_for_each_child(
        uacpi_namespace_root(), visit_thermal_zone, NULL, UACPI_OBJECT_THERMAL_ZONE_BIT,
        UACPI_MAX_DEPTH_ANY, &snapshot);

    snapshot.error = battery_status;
    if (!snapshot.error)
        snapshot.error = adapter_status;
    if (!snapshot.error)
        snapshot.error = thermal_status;
    power_ec_status(&snapshot.ec_count, &snapshot.ec_error);
    *out = snapshot;
    return snapshot.error;
}

/* Preparation runs AML and may sleep. Entry is separate so the kernel's
 * durable filesystem drain can remain between preparation and the S5 write.
 * A successful return from entry is not evidence that the machine powered off. */
int power_acpi_prepare_off(void)
{
    if (!namespace_ready)
        return -1;
    return (int)uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
}

int power_acpi_enter_off(void)
{
    if (!namespace_ready)
        return -1;
    return (int)uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
}

int power_acpi_reset(void)
{
    if (!namespace_ready)
        return -1;
    return (int)uacpi_reboot();
}

int power_acpi_restore_working(void)
{
    if (!namespace_ready)
        return -1;
    uacpi_status status = uacpi_prepare_for_wake_from_sleep_state(UACPI_SLEEP_STATE_S5);
    if (status == UACPI_STATUS_OK)
        status = uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S5);
    return (int)status;
}
