#ifndef POWER_ACPI_BUTTON_H
#define POWER_ACPI_BUTTON_H
/* Route the ACPI power button (the fixed event and any PNP0C0C device's
 * Notify 0x80) to an orderly kernel_poweroff(). The ACPI namespace must be
 * loaded and the caller holds the AML lock. 0 if at least one route is armed. */
int power_acpi_button_install(void);
#endif
