#include "pat.h"
#include "hardware.h"

static int g_pat_wc;   /* PA1 = WC programmed on the boot CPU */
int pat_wc_available(void) { return g_pat_wc; }

void pat_init(void)
{
    uint32_t r[4]; power_cpuid(1, 0, r);
    if (!(r[3] & (1u << 16)) || !(r[3] & (1u << 5))) return;
    /* Intel SDM vol.3A 11.12.4, Programming the PAT:
     * https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
     * Run on BSP BEFORE AP startup, and on each AP BEFORE it publishes online.
     * This is not a live global cache-type conversion: aliases and other CPUs
     * must already be quiescent. PA0 stays WB; only reset PA1 WT becomes WC.
     * Not verified on X79-VG4 hardware. */
    uint64_t flags = power_irq_save();
    uint64_t cr0 = power_read_cr(0), cr3 = power_read_cr(3), cr4 = power_read_cr(4);
    power_write_cr(0, (cr0 | (1ull << 30)) & ~(1ull << 29));
    power_wbinvd();
    power_write_cr(4, cr4 & ~(1ull << 7));
    power_write_cr(3, cr3);
    power_wrmsr(0x277, 0x0007040600070106ull);
    if (power_rdmsr(0x277) == 0x0007040600070106ull) g_pat_wc = 1;
    power_wbinvd();
    power_write_cr(3, cr3);
    power_write_cr(4, cr4);
    power_write_cr(0, cr0);
    power_irq_restore(flags);
}
