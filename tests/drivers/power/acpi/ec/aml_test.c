#include "board.h"
#include "devices.h"
#include <stdio.h>
#include <string.h>
#include <uacpi/uacpi.h>
extern unsigned char power_test_memory[16384];
static unsigned checks, failures;
#define CHECK(expression)                                                              \
    do {                                                                               \
        checks++;                                                                      \
        if (!(expression)) {                                                           \
            fprintf(stderr, "FAIL: %s\n", #expression);                                \
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
    power_ec_board_init(scenario);
    CHECK(power_acpi_start() == 0);
    struct power_acpi_status snapshot;
    CHECK(power_acpi_query(&snapshot) == 0);
    CHECK(snapshot.battery_count == 1);
    CHECK(snapshot.thermal_count == 1);
    int supported = !strcmp(scenario, "success") || !strcmp(scenario, "ecdt") ||
                    !strcmp(scenario, "timeout") || !strcmp(scenario, "pending-query");
    if (!supported) {
        CHECK(snapshot.ec_count == 0);
        CHECK(snapshot.ec_error != 0);
        CHECK(snapshot.batteries[0].status_error != 0);
        CHECK(snapshot.thermals[0].status_error != 0);
        CHECK(power_ec_model.port_writes == 0);
        goto done;
    }
    CHECK(snapshot.ec_count == 1);
    CHECK(power_ec_model.memory[0x10] == 0xa5);
    CHECK(power_ec_model.memory[0x11] == 1);
    uacpi_u64 registration_count = 0;
    CHECK(uacpi_eval_simple_integer(NULL, "\\_SB_.EC00.RGCN", &registration_count) ==
          UACPI_STATUS_OK);
    CHECK(registration_count == 1);
    if (!strcmp(scenario, "timeout")) {
        CHECK(snapshot.batteries[0].status_error != 0);
        CHECK(snapshot.thermals[0].status_error != 0);
        unsigned writes = power_ec_model.port_writes;
        CHECK(power_acpi_query(&snapshot) == 0);
        CHECK(snapshot.ec_error != 0);
        CHECK(power_ec_model.port_writes == writes);
        goto done;
    }
    if (!strcmp(scenario, "pending-query")) {
        CHECK(power_ec_board_early_gpe_enable() == 0);
        CHECK(power_ec_model.queries == 0);
        CHECK(power_ec_board_gpe_enabled() == 0);
        power_ec_board_drain();
        CHECK(power_ec_model.queries == 1);
        CHECK(power_ec_model.memory[0x12] == 0x5a);
        CHECK(power_ec_board_gpe_enabled() == 1);
        goto done;
    }
    CHECK(snapshot.ec_error == 0);
    CHECK(snapshot.batteries[0].status_error == 0);
    CHECK(snapshot.batteries[0].remaining == 4300);
    CHECK(snapshot.thermals[0].status_error == 0);
    CHECK(snapshot.thermals[0].temperature_millic == 26950);
    CHECK(power_ec_model.early_writes == 0);
    CHECK(power_ec_model.invalid_reads == 0);
    CHECK(power_ec_board_gpe_enabled() == 1);

    power_ec_model.memory[0x20] = 0xd0;
    CHECK(power_acpi_query(&snapshot) == 0);
    CHECK(snapshot.batteries[0].remaining == 4304);
    power_ec_board_raise_query(0x42);
    CHECK(power_ec_board_gpe_enabled() == 0);
    CHECK(power_ec_model.queries == 0);
    power_ec_board_drain();
    CHECK(power_ec_model.queries == 1);
    CHECK(power_ec_model.memory[0x12] == 0x5a);
    CHECK(power_ec_board_gpe_enabled() == 1);
    uacpi_u64 query_count = 0;
    CHECK(uacpi_eval_simple_integer(NULL, "\\_SB_.EC00.QCNT", &query_count) ==
          UACPI_STATUS_OK);
    CHECK(query_count == 1);
    power_ec_board_raise_query(0);
    power_ec_board_drain();
    CHECK(power_ec_model.queries == 2);
    CHECK(power_ec_board_gpe_enabled() == 1);
    CHECK(uacpi_eval_simple_integer(NULL, "\\_SB_.EC00.QCNT", &query_count) ==
          UACPI_STATUS_OK);
    CHECK(query_count == 1);
    CHECK(power_acpi_query(&snapshot) == 0);
    CHECK(snapshot.ec_error == 0);
done:
    /* Binding resources are boot-lifetime in production. Process exit releases
     * the board model; do not invent a driver unload protocol for this gate. */
    printf("EC_AML %s: %u checks, %u failures\n", scenario, checks, failures);
    return failures ? 1 : 0;
}
