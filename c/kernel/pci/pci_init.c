/* PCI bring-up: find the ECAM window in the ACPI MCFG table, then enumerate.
 *
 * Kept out of pci.c so that pci.c -- the part worth unit-testing on the host --
 * has no ACPI dependency. */
#include <stdint.h>
#include "pci.h"
#include "driver.h"
#include "acpi.h"
#include "kprintf.h"

int pci_ecam_init(void)
{
    uint64_t base; uint16_t seg; uint8_t lo, hi;
    for (int i = 0; acpi_mcfg_entry(i, &base, &seg, &lo, &hi) == 0; i++) {
        if (seg != 0) continue;                 /* only segment 0 for now */
        if (pci_ecam_set(base, seg, lo, hi)) {
            kprintf("[pci] ECAM seg %d bus %d-%d @ %p (MCFG)\n",
                    (int)seg, (int)lo, (int)hi, (void *)base);
            return 1;
        }
    }
    /* Not an error: i440fx (QEMU's default machine) has no MCFG at all, and the
     * legacy ports reach every bus of segment 0 -- they just cannot reach the
     * PCIe extended config space above offset 0xFF. */
    kprintf("[pci] no MCFG; legacy 0xCF8 config ports (no extended caps)\n");
    return 0;
}

int pci_init(void)
{
    pci_ecam_init();
    int n = pci_enumerate();
    kprintf("[pci] %d function(s) enumerated across all buses\n", n);
    return n;
}
