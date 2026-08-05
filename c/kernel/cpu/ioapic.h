#ifndef LOGIT_IOAPIC_H
#define LOGIT_IOAPIC_H
#include <stdint.h>

void ioapic_init(void);
int  ioapic_present(void);
void ioapic_route(uint32_t gsi, uint8_t vec, uint8_t apic_id, int level, int active_low);
void ioapic_route_isa(int isa_irq, uint8_t vec, uint8_t apic_id);

#endif
