#include "ioapic_map.h"

static struct ioapic_chip chips[IOAPIC_MAX_CHIPS];
static int chip_count;

/* Reset before discovery; callers must not erase a published runtime map. */
void ioapic_map_reset(void)
{
    chip_count = 0;
}

/* Intel 82093AA section 3: selectors 0x10+2*n and 0x11+2*n must both
 * fit in IOREGSEL's eight bits, permitting at most 120 entries. Use half-open
 * intervals for the overlap check, as Linux v6.12 io_apic.c mp_register_ioapic
 * does for mp_ioapic_gsi_routing; reject wrap before calculating the end. */
int ioapic_map_add(uint32_t phys, uint32_t gsi_base, unsigned nr_rte,
                   uint8_t id, uint8_t ver)
{
    if (!phys || (phys & 0xf) || nr_rte < 1 || nr_rte > 120 ||
        gsi_base > UINT32_MAX - nr_rte || chip_count == IOAPIC_MAX_CHIPS) {
        return -1;
    }
    for (int i = 0; i < chip_count; i++) {
        if (chips[i].phys == phys) {
            return -1;
        }
#ifndef PRT_NEGCTL_NO_OVERLAP_CHECK
        if (gsi_base < chips[i].gsi_base + chips[i].nr_rte &&
            chips[i].gsi_base < gsi_base + nr_rte) {
            return -1;
        }
#endif
    }
    chips[chip_count] = (struct ioapic_chip){phys, gsi_base, (uint16_t)nr_rte, id, ver};
    return chip_count++;
}

/* Expose discovery count without exposing mutable storage. */
int ioapic_map_count(void)
{
    return chip_count;
}

/* Diagnostics and selector changes share this bounds-checked view. */
const struct ioapic_chip *ioapic_map_chip(int idx)
{
    if (idx < 0 || idx >= chip_count) {
        return NULL;
    }
    return &chips[idx];
}

/* Runtime routing must use GSI coverage, never MADT order or a global base. */
int ioapic_map_find(uint32_t gsi)
{
    for (int i = 0; i < chip_count; i++) {
#ifdef PRT_NEGCTL_ONLY_FIRST_IOAPIC
        if (i >= 1) {
            break;
        }
#endif
        if (gsi >= chips[i].gsi_base && gsi - chips[i].gsi_base < chips[i].nr_rte) {
            return i;
        }
    }
    return -1;
}
