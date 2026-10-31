/* Board bring-up: SMBIOS identity, TCO watchdog, Super I/O, CF9 reset.
 * See board.h. The pieces that decide anything live in smbios.c and pch_lpc.c,
 * which the host gate drives with a synthesized model of the X79-VG4 built
 * from its Windows inventory (IDs and strings, not captured registers); this
 * file only reports
 * and hands the reset to the power path. The power button is ACPI and lives
 * in c/drivers/power/acpi/button.c. */
#include <stddef.h>
#include <stdint.h>
#include "board.h"
#include "smbios.h"
#include "pch_lpc.h"
#include "board_io.h"
#include "intel_imc_smbus.h"
#include "nct6779.h"
#include "hwreport.h"

/* Filled once by board_early(), before any other thread exists, and only
 * read afterwards; that ordering is the synchronization. */
static struct pch_lpc g_lpc;
static int g_lpc_found;
static struct tco_report g_tco;

static const struct pch_lpc *lpc(void) { return g_lpc_found ? &g_lpc : NULL; }

static void report_tco(void)
{
    const struct tco_report *t = &g_tco;
    unsigned secs = (unsigned)t->ticks * 6 / 10;
    switch (t->state) {
    case TCO_ABSENT:      kprintf("[board] tco: not checked (no Intel LPC or ACPI decode off)\n"); break;
    case TCO_HALTED:      kprintf("[board] tco halted by firmware (base=0x%x)\n", t->tcobase); break;
    case TCO_WAS_RUNNING: kprintf("[board] tco WAS RUNNING (%u ticks, ~%u s) -- halted\n", t->ticks, secs); break;
    case TCO_STUCK:       kprintf("[board] tco running and refused to halt -- a second timeout in ~%u s resets the board unless NO_REBOOT is set\n", secs * 2); break;
    }
    if (t->second_timeout)
        kprintf("[board] tco: TCO2_STS.SECOND_TO_STS is set: a second watchdog timeout happened since the flag "
                "was last cleared (it survives resets; NO_REBOOT may have suppressed the reset itself)\n");
}

void board_early(void)
{
    g_lpc_found = pch_lpc_find(&g_lpc) == 0;   /* zeroes g_lpc when absent */
    tco_quiesce(&g_lpc, &g_tco);
    report_tco();
}

static void report_smbios(void)
{
    static struct smbios_info si;       /* ~1 KiB: not on the init thread's stack */
    int rc = smbios_read(&si);
    if (rc) {
        kprintf("[board] smbios not found (rc=%d)\n", rc);
        return;
    }
    kprintf("[board] smbios %u.%u\n", si.major, si.minor);
    kprintf("[board] baseboard \"%s\" \"%s\" \"%s\"\n", si.board_vendor, si.board_product, si.board_version);
    kprintf("[board] system \"%s\" \"%s\"\n", si.sys_vendor, si.sys_product);
    kprintf("[board] bios \"%s\" \"%s\" \"%s\"\n", si.bios_vendor, si.bios_version, si.bios_date);
    for (int i = 0; i < si.dimm_count; i++) {
        const struct smbios_dimm *d = &si.dimm[i];
        kprintf("[board] dimm \"%s\" \"%s\" %u MiB %u MT/s \"%s\" \"%s\"\n",
                d->locator, d->bank, d->size_mb, d->speed_mts, d->vendor, d->part);
    }
}

void board_init(void)
{
    report_smbios();
    imc_spd_scan();
    if (imc_spd_inventory()->modules)
        hwreport_milestone("spd", "OBSERVED DIMMs read over the CPU iMC SMBus");

    const struct pch_lpc *l = lpc();
    if (!l) {
        kprintf("[board] lpc: no Intel PCH LPC bridge at 00:1f.0\n");
        return;
    }
    kprintf("[board] lpc 8086:%04x %s acpibase=0x%x lpc_en=0x%04x\n",
            l->device, l->family, l->acpi_base, l->lpc_en);

    report_tco();                         /* quiesced long before, in board_early() */

    struct sio_chip c[SIO_MAX];
    int n = superio_probe(l, c);
    if (!n) kprintf("[board] superio none on the forwarded ports (lpc_en=0x%04x)\n", l->lpc_en);
    for (int i = 0; i < n; i++) {
        kprintf("[board] superio 0x%x: %s %s (id %04x)\n", c[i].port, c[i].vendor,
                c[i].name ? c[i].name : "unknown chip", c[i].id);
        struct nct_sensors s;
        if ((c[i].id & 0xfff8) != 0xc560) continue;
        if (nct6779_find(c[i].port) != 0 || nct6779_read(&s) != 0) {
            kprintf("[hwmon] NCT6779 monitor disabled or absent; not enabled\n");
            continue;
        }
        kprintf("[hwmon] NCT6779 at 0x%x:", s.base);
        for (int t = 0; t < NCT_TEMPS; t++)
            if (s.temp_c[t] != -128) kprintf(" %s %dC", nct_temp_label[t], s.temp_c[t]);
        for (int f = 0; f < NCT_FANS; f++)
            if (s.fan_rpm[f]) kprintf(" fan%d %urpm", f + 1, s.fan_rpm[f]);
        kprintf(" CPUVC %umV 3VCC %umV VBAT %umV\n", s.mv[0], s.mv[3], s.mv[8]);
    }
}

void board_reset(void)
{
    const struct pch_lpc *l = lpc();
    if (!l) return;
    kprintf("[power] reboot: PCH 0xCF9 cold reset\n");
    pch_reset(l);
    kprintf("[power] reboot: 0xCF9 did not take\n");
}
