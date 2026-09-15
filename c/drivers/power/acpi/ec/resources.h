#ifndef LOGIT_POWER_EC_RESOURCES_H
#define LOGIT_POWER_EC_RESOURCES_H
#include <stdint.h>
#include <uacpi/types.h>

struct ec_resources {
    uint16_t data_port;
    uint16_t control_port;
    uint16_t gpe;
};
/* Resolve only an exclusive, 16-bit-decoded SystemIO EC with a global GPE.
 * ECDT, when it identifies this device, must agree with its namespace data. */
uacpi_status ec_resources_from_device(uacpi_namespace_node *node,
                                      struct ec_resources *out);
#endif
