/* ACPI ownership check for the NCT HWM I/O window. Kept separate from the
 * port driver so namespace inspection never occurs under an IRQ spinlock. */
#ifndef NCT_ACPI_H
#define NCT_ACPI_H

#include <stdint.h>

struct nct_io_region {
    uint16_t start;
    uint16_t len;
};

/* Half-open byte ranges; widened endpoints avoid wrapping at port 0xFFFF. */
int nct_region_conflict(const struct nct_io_region *regions, int count,
                        uint16_t base, uint16_t length);

/* After AML initialization, return zero only after a complete namespace scan.
 * A conflict refuses access with -EBUSY; an incomplete scan returns -EIO. */
int nct_acpi_check(uint16_t base, uint16_t length);

#endif
