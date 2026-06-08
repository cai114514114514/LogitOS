#ifndef AETHER_LAPIC_H
#define AETHER_LAPIC_H
#include <stdint.h>

void     lapic_init(void);                          /* enable this CPU's LAPIC */
int      lapic_ready(void);                          /* 1 once LAPIC MMIO is mapped */
uint32_t lapic_id(void);                            /* this CPU's APIC ID */
void     lapic_eoi(void);
void     lapic_start_ap(uint8_t apic_id, uint8_t trampoline_vec);
void     lapic_send_ipi(uint8_t apic_id, uint8_t vec);
void     lapic_timer_init(uint8_t vec, uint32_t count);

#endif
