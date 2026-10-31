#ifndef LOGIT_IOAPIC_MAP_H
#define LOGIT_IOAPIC_MAP_H
#include "madt_irq.h"

#define IOAPIC_MAX_CHIPS ACPI_MAX_IOAPICS
struct ioapic_chip {
    uint32_t phys;
    uint32_t gsi_base;
    uint16_t nr_rte;
    uint8_t id;
    uint8_t ver;
};

/* Boot-only writers, immutable readers after ioapic_ready publication. */
void ioapic_map_reset(void);
int ioapic_map_add(uint32_t phys, uint32_t gsi_base, unsigned nr_rte,
                   uint8_t id, uint8_t ver);
int ioapic_map_count(void);
const struct ioapic_chip *ioapic_map_chip(int idx);
int ioapic_map_find(uint32_t gsi);
#endif
