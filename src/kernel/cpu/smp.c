/* SMP bring-up.
 * Stage 1: detect CPUs (ACPI) + enable the BSP's LAPIC.
 * Stage 2: copy the AP trampoline to 0x8000 and INIT-SIPI each AP into long
 *          mode; APs report in, enable their LAPIC, then park (hlt). */
#include <stdint.h>
#include <stddef.h>
#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "vmm.h"
#include "pit.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *kmalloc(unsigned long);

extern uint8_t ap_tramp_start[], ap_tramp_end[];

#define TRAMP_PHYS 0x8000
#define AP_ARGS    ((volatile uint64_t *)0x8F00)   /* cr3 @0, stack @1, entry @2 */
#define AP_STACK   (64 * 1024)

static volatile int g_online = 1;     /* BSP */
static volatile int ap_ack;           /* AP -> BSP handshake during bringup */

int smp_cpu_count(void) { return g_online; }

/* First C code each application processor runs (Stage 2: report + park). */
static void ap_entry(void)
{
    lapic_init();                     /* enable this CPU's local APIC */
    int id = (int)lapic_id();
    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    kprintf("[smp] CPU apic_id=%d online\n", id);
    ap_ack = 1;                       /* tell the BSP we made it */
    for (;;) __asm__ volatile ("cli; hlt");   /* park (Stage 3 will schedule here) */
}

void smp_init(void)
{
    int n = acpi_init();
    lapic_init();                     /* BSP LAPIC */
    if (n < 1) { kprintf("[smp] no CPUs via ACPI; uniprocessor\n"); return; }
    kprintf("[smp] %d CPU(s) detected, BSP apic_id=%d\n", n, (int)lapic_id());

    /* copy the trampoline into low memory (identity-mapped, < 1 MiB) */
    memcpy((void *)TRAMP_PHYS, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));
    AP_ARGS[0] = vmm_kernel_cr3();    /* shared kernel address space */

    uint32_t bsp = lapic_id();
    for (int i = 0; i < n; i++) {
        uint8_t aid = acpi_cpu_apic_id(i);
        if (aid == bsp) continue;
        uint8_t *stk = kmalloc(AP_STACK);
        if (!stk) continue;
        AP_ARGS[1] = (uint64_t)(stk + AP_STACK) & ~(uint64_t)0xF;
        AP_ARGS[2] = (uint64_t)ap_entry;
        ap_ack = 0;
        lapic_start_ap(aid, TRAMP_PHYS >> 12);   /* INIT-SIPI-SIPI */
        for (volatile long w = 0; !ap_ack && w < 200000000L; w++) __asm__ volatile ("pause");
        if (!ap_ack) kprintf("[smp] CPU apic_id=%d did not start\n", (int)aid);
    }
    kprintf("[smp] %d/%d CPUs online\n", g_online, n);
}
