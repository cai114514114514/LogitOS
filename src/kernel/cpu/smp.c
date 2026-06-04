/* SMP bring-up + a parallel framebuffer present.
 *
 * Stage 1: detect CPUs (ACPI) + enable the BSP's LAPIC.
 * Stage 2: INIT-SIPI each AP into long mode on the shared kernel address space.
 * Stage 3: APs idle on hlt and wake via a work IPI (vector 240) to copy a
 *          disjoint band of the back buffer to the framebuffer. The full-screen
 *          present (the costly part of every frame) now runs on all cores.
 *          This is safe without locks: the source (back buffer) is read-only and
 *          each CPU writes a disjoint set of rows. No scheduler/kernel changes,
 *          so the rest of the system is exactly as before. */
#include <stdint.h>
#include <stddef.h>
#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "idt.h"
#include "pic.h"
#include "vmm.h"
#include "fb.h"
#include "e1000.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *kmalloc(unsigned long);

extern uint8_t ap_tramp_start[], ap_tramp_end[];

#define TRAMP_PHYS 0x8000
#define AP_ARGS    ((volatile uint64_t *)0x8F00)   /* cr3 @0, stack @1, entry @2 */
#define AP_STACK   (64 * 1024)
#define MAXCPU     ACPI_MAX_CPUS
#define IPI_WORK   240

static uint8_t  cpu_apicid[MAXCPU];     /* index -> APIC id; index 0 = BSP */
static volatile int g_online = 1;       /* CPUs online (incl. BSP) */
static volatile int ap_ack;             /* bringup handshake */
static volatile int g_via_apic;         /* device IRQs go through the I/O APIC */

int smp_irq_via_apic(void) { return g_via_apic; }

struct band { volatile int done; int x, y, w, h; };
static struct band g_band[MAXCPU];

int smp_cpu_count(void) { return g_online; }

static int this_index(void)
{
    uint32_t id = lapic_id();
    for (int i = 0; i < g_online; i++) if (cpu_apicid[i] == id) return i;
    return 0;
}

/* Called from the vector-240 IPI handler: copy this CPU's assigned band. */
void smp_ipi_work(void)
{
    int i = this_index();
    fb_copy_rect(g_band[i].x, g_band[i].y, g_band[i].w, g_band[i].h);
    __atomic_store_n(&g_band[i].done, 1, __ATOMIC_SEQ_CST);
}

/* Parallel back->framebuffer present: split [y,y+h) rows across all CPUs. */
static void smp_present(int x, int y, int w, int h)
{
    int n = g_online;
    if (n <= 1) { fb_copy_rect(x, y, w, h); return; }
    int rh = h / n;
    for (int i = 1; i < n; i++) {
        g_band[i].x = x; g_band[i].w = w;
        g_band[i].y = y + i * rh;
        g_band[i].h = (i == n - 1) ? (h - i * rh) : rh;
        __atomic_store_n(&g_band[i].done, 0, __ATOMIC_SEQ_CST);
        lapic_send_ipi(cpu_apicid[i], IPI_WORK);
    }
    fb_copy_rect(x, y, w, rh);                         /* BSP does band 0 */
    for (int i = 1; i < n; i++) {
        long spins = 0;
        while (!__atomic_load_n(&g_band[i].done, __ATOMIC_SEQ_CST)) {
            __asm__ volatile ("pause");
            if (++spins > 50000000L) {                 /* AP stalled -> BSP finishes its band */
                fb_copy_rect(g_band[i].x, g_band[i].y, g_band[i].w, g_band[i].h);
                break;
            }
        }
    }
}

/* First C code each AP runs: enable LAPIC, load the shared IDT, then idle
 * waiting for work IPIs. */
static void ap_entry(void)
{
    lapic_init();
    idt_load();
    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    kprintf("[smp] CPU apic_id=%d online\n", (int)lapic_id());
    ap_ack = 1;
    for (;;) __asm__ volatile ("sti; hlt");            /* woken by vector-240 IPIs */
}

void smp_init(void)
{
    int n = acpi_init();
    lapic_init();
    if (n < 1) { kprintf("[smp] no CPUs via ACPI; uniprocessor\n"); return; }
    kprintf("[smp] %d CPU(s) detected, BSP apic_id=%d\n", n, (int)lapic_id());

    /* Switch device IRQs from the legacy PIC to the I/O APIC: route the ISA
     * lines (timer/keyboard/mouse) to the BSP and mask the PIC. EOI then goes
     * to the LAPIC (see interrupts.c). */
    ioapic_init();
    if (ioapic_present()) {
        uint8_t bspid = (uint8_t)lapic_id();
        ioapic_route_isa(0,  32, bspid);     /* PIT timer  -> vec 32 */
        ioapic_route_isa(1,  33, bspid);     /* keyboard   -> vec 33 */
        ioapic_route_isa(12, 44, bspid);     /* PS/2 mouse -> vec 44 */
        int nicg = e1000_irq_line();         /* e1000 NIC -> vec 65 (PCI: level, active-low) */
        if (nicg > 0 && nicg < 24) ioapic_route((uint32_t)nicg, 65, bspid, 1, 1);
        pic_disable();
        g_via_apic = 1;
        kprintf("[ioapic] device IRQs routed via I/O APIC (NIC gsi=%d)\n", nicg);
    }

    memcpy((void *)TRAMP_PHYS, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));
    AP_ARGS[0] = vmm_kernel_cr3();

    uint32_t bsp = lapic_id();
    cpu_apicid[0] = (uint8_t)bsp;
    for (int i = 0; i < n; i++) {
        uint8_t aid = acpi_cpu_apic_id(i);
        if (aid == bsp) continue;
        uint8_t *stk = kmalloc(AP_STACK);
        if (!stk) continue;
        cpu_apicid[g_online] = aid;        /* claim the next CPU index */
        AP_ARGS[1] = (uint64_t)(stk + AP_STACK) & ~(uint64_t)0xF;
        AP_ARGS[2] = (uint64_t)ap_entry;
        ap_ack = 0;
        lapic_start_ap(aid, TRAMP_PHYS >> 12);
        for (volatile long w = 0; !ap_ack && w < 200000000L; w++) __asm__ volatile ("pause");
        if (!ap_ack) { cpu_apicid[g_online] = 0; kprintf("[smp] CPU apic_id=%d did not start\n", (int)aid); }
    }
    kprintf("[smp] %d/%d CPUs online\n", g_online, n);

    if (g_online > 1) {
        /* quick benchmark: serial vs parallel full-screen present (rdtsc cycles) */
        extern uint32_t fb_width(void), fb_height(void);
        int W = (int)fb_width(), H = (int)fb_height();
        uint64_t a0, a1, b0, b1, lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); a0 = lo | (hi << 32);
        for (int k = 0; k < 16; k++) fb_copy_rect(0, 0, W, H);
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); a1 = lo | (hi << 32);
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); b0 = lo | (hi << 32);
        for (int k = 0; k < 16; k++) smp_present(0, 0, W, H);
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); b1 = lo | (hi << 32);
        uint64_t ser = (a1 - a0) / 16, par = (b1 - b0) / 16;
        kprintf("[smp] present: serial=%d kcyc  parallel=%d kcyc  (%dx on %d cores)\n",
                (int)(ser / 1000), (int)(par / 1000), (int)(par ? ser / par : 0), g_online);
        fb_set_present_par(smp_present);   /* full-screen present now runs on all cores */
    }
}
