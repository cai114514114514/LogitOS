#ifndef LOGIT_IOAPIC_H
#define LOGIT_IOAPIC_H
#include <stdint.h>

enum {
    IOAPIC_ROUTE_OK = 0,
    IOAPIC_ROUTE_SAFE_REJECT = -1, /* operation failed; RTE is confirmed masked */
    IOAPIC_ROUTE_UNSAFE = -2,      /* operation failed; masking could not be confirmed */
};

/* Boot-only discovery/mapping; runtime route updates use a short controller lock. */
int  ioapic_init(void);
int  ioapic_present(void);
int  ioapic_gsi_valid(uint32_t gsi);
int  ioapic_can_route(uint32_t gsi, uint32_t apic_id);
/* Mask and read back the RTE before retiring its vector. Returns 0 on an
 * acknowledged mask, -1 for an absent GSI or failed register readback. */
int  ioapic_mask(uint32_t gsi);
int  ioapic_is_masked(uint32_t gsi); /* 1 masked, 0 enabled, -1 absent */
int  ioapic_route(uint32_t gsi, uint8_t vec, uint32_t apic_id, int level, int active_low);
int  ioapic_route_isa(int isa_irq, uint8_t vec, uint32_t apic_id);
/* Program timer/keyboard/mouse as one boot transaction. A failed transaction
 * masks and verifies every member before allowing the PIC fallback. */
int  ioapic_route_legacy_set(uint32_t apic_id);

#endif
