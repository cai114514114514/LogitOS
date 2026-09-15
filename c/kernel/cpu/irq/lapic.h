#ifndef LOGIT_LAPIC_H
#define LOGIT_LAPIC_H
#include <stdint.h>

int      lapic_init(void);                           /* preserve + initialize handed-off mode */
int      lapic_ready(void);                          /* MMIO xAPIC or MSR x2APIC live */
uint32_t lapic_id(void);                            /* this CPU's APIC ID */
void     lapic_eoi(void);
int      lapic_start_ap(uint32_t apic_id, uint8_t trampoline_vec);
int      lapic_send_ipi(uint32_t apic_id, uint8_t vec);
int      lapic_ipi_destination_supported(uint32_t apic_id);
void     lapic_timer_init(uint8_t vec, uint32_t count);
int      lapic_x2apic_active(void);
const char *lapic_mode_name(void);

#ifdef LOGIT_LAPIC_HOST
void lapic_host_reset(void);
#endif

#endif
