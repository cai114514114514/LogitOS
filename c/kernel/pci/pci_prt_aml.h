#ifndef LOGIT_PCI_PRT_AML_H
#define LOGIT_PCI_PRT_AML_H
#include <stdint.h>
#include "pci_route.h"

struct pci_prt_topology {
    int (*bridge_of)(void *ctx, uint8_t bus, uint8_t *parent_bus,
                     uint8_t *slot, uint8_t *func);
    void *ctx;
};

/* Called after namespace load and before _INI; PIC fallback makes no AML call. */
int pci_prt_aml_set_model(int ioapic);
int pci_prt_aml_preinit(void);
int pci_prt_aml_resolve(const struct pci_prt_topology *topology,
                       const struct device *device, struct pci_irq_route *out);
void pci_prt_aml_dump(const struct pci_prt_topology *topology);
void pci_prt_aml_ready(void);
#endif
