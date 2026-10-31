#ifndef LOGIT_PCI_ROUTE_H
#define LOGIT_PCI_ROUTE_H
#include <stdint.h>

struct device;
enum {
    PCI_ROUTE_NONE = 0,
    PCI_ROUTE_AML = 1,
    PCI_ROUTE_RCBA = 2,
    PCI_ROUTE_FIRMWARE = 3
};
struct pci_irq_route {
    uint32_t gsi;
    uint8_t level;
    uint8_t active_low;
    uint8_t source;
    uint8_t pin;             /* zero-based pin at the matching bus */
    uint8_t hops;
    char how[48];
};
typedef int (*pci_route_resolver_fn)(const struct device *, struct pci_irq_route *);
struct pci_reroute_stats {
    unsigned devices, same, moved, kept, failed, lost;
};

/* One INTx binding epoch. Deliveries count dispatcher entries for this member,
 * never timer/catchup callbacks. A shared wire still needs ownership checks:
 * neither PCI nor the IOAPIC identifies which member asserted it. */
struct pci_intx_delivery {
    uint64_t generation;
    uint64_t count;
    int vector;
};

/* Copy all fields under the INTx gate; return -1 for absent/retiring members.
 * A caller publishing evidence must own bind_busy through publication so a
 * coherent but already retired sample cannot certify a later binding. */
int pci_intx_delivery_snapshot(const struct device *device, struct pci_intx_delivery *out);

/* AML is registered only after namespace initialization, keeping early probe
 * and standalone PCI host tests independent of the interpreter. */
void pci_intx_set_aml_resolver(pci_route_resolver_fn fn);
const char *pci_route_source_name(int source);
/* Supply a sleep in the caller's worker context, outside all routing locks.
 * Standalone PCI probes need no scheduler dependency. NULL disables deferred
 * retry; the AML worker installs its 5 ms sleep before the one-shot pass. */
void pci_intx_set_reroute_wait(void (*wait)(void));
void pci_intx_reroute_all(struct pci_reroute_stats *stats);
int pci_intx_route_of(const struct device *device, struct pci_irq_route *out);
int pci_route_diag(int index, char *buffer, int max);
#endif
