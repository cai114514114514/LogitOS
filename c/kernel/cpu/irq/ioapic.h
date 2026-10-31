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
int  ioapic_chip_count(void);
int  ioapic_describe(int chip, char *buf, int max);
int  ioapic_gsi_valid(uint32_t gsi);
int  ioapic_can_route(uint32_t gsi, uint32_t apic_id);
/* Mask, read back, and drain an old delivery before retiring its vector.
 * Returns 0 only with Mask set and Remote IRR/Delivery Status clear, -1 for
 * an absent GSI, failed mask, or stuck delivery (keep that vector isolated).
 * Linux v6.12 io_apic.c: ioapic_irq_get_chip_state(), __eoi_ioapic_pin(). */
int  ioapic_mask(uint32_t gsi);
int  ioapic_is_masked(uint32_t gsi); /* 1 masked, 0 enabled, -1 absent */
uint32_t ioapic_rte_low(uint32_t gsi);  /* diagnostics: the low RTE word */
int  ioapic_route(uint32_t gsi, uint8_t vec, uint32_t apic_id, int level, int active_low);
int  ioapic_route_isa(int isa_irq, uint8_t vec, uint32_t apic_id);
/* Program timer/keyboard/mouse as one boot transaction. A failed transaction
 * masks and verifies every member before allowing the PIC fallback. */
int  ioapic_route_legacy_set(uint32_t apic_id);

#endif
