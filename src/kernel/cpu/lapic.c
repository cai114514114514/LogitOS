/* Local APIC: per-CPU interrupt controller. We use it to (a) bring up the APs
 * via INIT-SIPI-SIPI IPIs and (b) drive a per-CPU preemption timer. Device IRQs
 * (PIT/keyboard/mouse/NIC) still go through the legacy PIC to the BSP. */
#include <stdint.h>
#include "lapic.h"
#include "acpi.h"
#include "vmm.h"

static volatile uint8_t *lapic;     /* mapped MMIO base */

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)(lapic + reg); }
static inline void     wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(lapic + reg) = v; }

#define LAPIC_ID     0x020
#define LAPIC_EOI    0x0B0
#define LAPIC_SVR    0x0F0      /* spurious-interrupt vector register */
#define LAPIC_ICRLO  0x300
#define LAPIC_ICRHI  0x310
#define LAPIC_LVT_TMR 0x320
#define LAPIC_TMRINIT 0x380
#define LAPIC_TMRCUR  0x390
#define LAPIC_TMRDIV  0x3E0

/* short busy spin (~microseconds); IPI inter-command gaps are forgiving */
static void spin(volatile int n) { while (n-- > 0) __asm__ volatile ("pause"); }

void lapic_init(void)
{
    if (!lapic) {
        uint64_t base = acpi_lapic_base();
        vmm_map_page(base, base, VMM_WRITABLE | VMM_NOCACHE);   /* MMIO, uncached */
        lapic = (volatile uint8_t *)base;
    }
    wr(LAPIC_SVR, 0x100 | 0xFF);    /* bit 8 = APIC enable; spurious vector 0xFF */
    wr(LAPIC_EOI, 0);
}

uint32_t lapic_id(void) { return rd(LAPIC_ID) >> 24; }
void     lapic_eoi(void) { wr(LAPIC_EOI, 0); }

static void ipi_wait(void) { while (rd(LAPIC_ICRLO) & (1 << 12)) ; }   /* delivery status */

/* INIT-SIPI-SIPI: standard AP wake sequence. `vec` = trampoline page number. */
void lapic_start_ap(uint8_t apic_id, uint8_t vec)
{
    wr(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    wr(LAPIC_ICRLO, 0x00004500);    /* INIT, assert, edge */
    ipi_wait();
    spin(200000);                   /* ~10 ms */
    for (int i = 0; i < 2; i++) {   /* two STARTUP IPIs */
        wr(LAPIC_ICRHI, (uint32_t)apic_id << 24);
        wr(LAPIC_ICRLO, 0x00004600 | vec);   /* STARTUP, vector = vec */
        spin(5000);                 /* ~200 us */
        ipi_wait();
    }
}

/* Fixed IPI to one CPU (e.g. wake an AP to run a work item). */
void lapic_send_ipi(uint8_t apic_id, uint8_t vec)
{
    wr(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    wr(LAPIC_ICRLO, 0x00004000 | vec);     /* fixed delivery, physical, assert */
    ipi_wait();
}

/* Periodic LAPIC timer -> `vec` every ~`hz` Hz (rough; not calibrated). */
void lapic_timer_init(uint8_t vec, uint32_t count)
{
    wr(LAPIC_TMRDIV, 0x3);                  /* divide by 16 */
    wr(LAPIC_LVT_TMR, vec | (1 << 17));     /* periodic mode */
    wr(LAPIC_TMRINIT, count);
}
