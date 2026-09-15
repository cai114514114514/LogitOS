#include "devices.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uacpi/uacpi.h>
extern unsigned char power_test_memory[16384];
static unsigned checks;
static unsigned failures;
#define CHECK(condition)                                                               \
    do {                                                                               \
        checks++;                                                                      \
        if (!(condition)) {                                                            \
            fprintf(stderr, "FAIL: %s\n", #condition);                                 \
            failures++;                                                                \
        }                                                                              \
    } while (0)

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *file = fopen(argv[2], "rb");
    if (!file || fread(power_test_memory, 1, sizeof power_test_memory, file) !=
                     sizeof power_test_memory)
        return 2;
    fclose(file);
    const char *scenario = argv[1];
    int started = power_acpi_start();
    if (!strcmp(scenario, "bad-checksum")) {
        CHECK(started != 0);
        goto done;
    }
    CHECK(started == 0);
    if (started)
        goto done;

    struct power_acpi_status snapshot;
    CHECK(power_acpi_query(&snapshot) == 0);
    CHECK(snapshot.ready == 1);
    CHECK(snapshot.battery_count == 1);
    CHECK(snapshot.thermal_count == 1);
    CHECK(snapshot.ac_online == 1);
    CHECK(snapshot.truncated == 0);
    struct power_battery *battery = &snapshot.batteries[0];
    struct power_thermal *thermal = &snapshot.thermals[0];
    CHECK(strstr(battery->path, "BAT0") != NULL);
    if (!strcmp(scenario, "empty")) {
        CHECK(battery->present == 0);
        CHECK(battery->remaining == POWER_UNKNOWN);
    } else {
        CHECK(battery->present == 1);
        CHECK(battery->info_error == 0);
        CHECK(battery->unit == 0);
        CHECK(battery->full == 4800);
        CHECK(battery->design_voltage == 12000);
        if (!strcmp(scenario, "bad-battery") ||
            !strcmp(scenario, "missing-battery-method")) {
            CHECK(battery->status_error != 0);
            CHECK(battery->remaining == POWER_UNKNOWN);
        } else {
            CHECK(battery->status_error == 0);
            CHECK(battery->state == 1);
            CHECK(battery->rate == 1000);
            CHECK(battery->remaining ==
                  (!strcmp(scenario, "unknown") ? POWER_UNKNOWN : 4200));
        }
    }
    CHECK(thermal->critical_valid == 1);
    CHECK(thermal->critical_millic == 100050);
    if (!strcmp(scenario, "bad-temperature")) {
        CHECK(thermal->status_error != 0);
    } else {
        CHECK(thermal->status_error == 0);
        CHECK(thermal->temperature_millic == 26950);
        CHECK(power_acpi_query(&snapshot) == 0);
        CHECK(thermal->temperature_millic == 27050);
    }
    if (!strcmp(scenario, "bad-sleep"))
        CHECK(power_acpi_prepare_off() != 0);
    else
        CHECK(power_acpi_prepare_off() == 0);
    CHECK(power_acpi_query(NULL) != 0);
done:
    uacpi_state_reset();
    printf("POWER_AML %s: %u checks, %u failures\n", scenario, checks, failures);
    return failures ? 1 : 0;
}
