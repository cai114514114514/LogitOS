#ifndef LOGIT_POWER_ACPI_DEVICES_H
#define LOGIT_POWER_ACPI_DEVICES_H

#include <stddef.h>
#include <stdint.h>

#define POWER_BATTERIES 8
#define POWER_THERMALS 16
#define POWER_UNKNOWN UINT32_MAX

struct power_battery {
    char path[96];
    int present;
    int status_error; /* uACPI status from _STA/_BST; zero means no error. */
    int info_error;   /* uACPI status from _BIF. */

    uint32_t state;
    uint32_t rate;
    uint32_t remaining;
    uint32_t voltage;
    uint32_t unit; /* ACPI: 0 = mWh/mW, 1 = mAh/mA; voltage is always mV. */
    uint32_t design;
    uint32_t full;
    uint32_t design_voltage;
};

struct power_thermal {
    char path[96];
    int status_error;
    int32_t temperature_millic;
    int critical_valid;
    int32_t critical_millic;
};

struct power_acpi_status {
    int ready;
    int error; /* Namespace initialization/walk status, independent of methods. */
    int battery_count;
    int thermal_count;
    int truncated;
    unsigned ec_count;
    int ec_error; /* Rejected binding or transport/query failure; zero if none. */
    int ac_online; /* -1 means no successful _PSR, never an invented AC supply. */
    struct power_battery batteries[POWER_BATTERIES];
    struct power_thermal thermals[POWER_THERMALS];
};

/* Called in sleepable context after native services are ready. The caller must
 * serialize all these entry points. A query replaces the entire snapshot;
 * individual method failures have explicit error fields and unknown values.
 * A battery's rate/capacity unit is usable only when info_error is zero. */
int power_acpi_start(void);
int power_acpi_query(struct power_acpi_status *out);

/* Preparation evaluates firmware AML and may sleep. The kernel consumer owns
 * filesystem draining and interrupt state around the separate entry step. */
int power_acpi_prepare_off(void);
int power_acpi_enter_off(void);
int power_acpi_reset(void);
int power_acpi_restore_working(void);

#endif
