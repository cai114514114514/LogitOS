#ifndef LOGIT_POWER_EC_DRIVER_H
#define LOGIT_POWER_EC_DRIVER_H
#include <uacpi/types.h>

/* namespace_load -> EC handler installation/_REG -> namespace_initialize.
 * No EC is a successful empty discovery; unsupported ECs retain an error
 * diagnostic without pretending their OperationRegions can be evaluated. */
uacpi_status power_ec_initialize(void);
/* Enable query delivery only after _INI processing has completed. */
uacpi_status power_ec_activate(void);
void power_ec_status(unsigned *bound, int *error);
#endif
