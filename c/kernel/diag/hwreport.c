/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hwreport.h"
#include "hwreport_log.h"
#include "kprintf.h"
#include "driver.h"
#include "smp.h"
#include "pmm.h"
#include "blkdev.h"
#include "intel_pch_smbus.h"
#include <stdarg.h>
#include <string.h>

static const char *const names[] = {
    "efi", "fb", "smp", "root", "input", "desktop", "net", "audio", "spd", "power"
};
enum { LEVELS = sizeof names / sizeof names[0], ROOT_LEVEL = 3, DETAIL_BYTES = 160 };
static char details[LEVELS][DETAIL_BYTES];
/* Kept independently of klog's overwrite ring: early milestones must survive
 * a verbose probe pass. 64 PCI functions + 8 DIMMs + ten ladder rows fit well
 * below 64 KiB. Reserve a final overflow row rather than truncate a row into
 * plausible-looking data. Both sinks receive the SAME bytes, including that
 * explicit limit. No heap, disk or driver probing occurs on the early path. */
static char transcript[65536];
static int used, overflow, dumped, root_ready, save_failed;

static void emit(const char *fmt, ...)
{
    if (overflow) return;
    char line[384];
    int n = ksnprintf(line, sizeof line, "[hw] ");
    va_list ap;
    va_start(ap, fmt);
    n += kvsnprintf(line + n, (int)sizeof line - n - 1, fmt, ap);
    va_end(ap);
    /* A detail/part number with a newline must not escape the [hw] prefix or
     * impersonate a boot marker. Sanitize every formatted row, not just callers. */
    for (int i = 5; i < n; i++)
        if ((unsigned char)line[i] < 32 || line[i] == 127) line[i] = ' ';
    line[n++] = '\n';
    line[n] = 0;
    if (used + n > (int)sizeof transcript - 80) {
        n = ksnprintf(line, sizeof line, "[hw] report TRUNCATED transcript capacity reached\n");
        overflow = 1;
    }
    memcpy(transcript + used, line, (size_t)n);
    used += n;
    kprintf("%s", line);
}

static void save_report(void)
{
    if (!root_ready || save_failed) return;
    if (hwreport_save(transcript, used) < 0) {
        save_failed = 1;
        emit("log unavailable: filesystem refused persistence; serial retained");
    }
}

void hwreport_milestone(const char *id, const char *detail)
{
    if (!id) return;
    for (unsigned i = 0; i < LEVELS; i++) {
        if (strcmp(id, names[i])) continue;
        ksnprintf(details[i], sizeof details[i], "%s", detail ? detail : "NOT_OBSERVED");
        if (i == ROOT_LEVEL)
            root_ready = details[i][0] == 'O' && details[i][1] == 'K' &&
                         (!details[i][2] || details[i][2] == ' ');
        emit("milestone %s %s", names[i], details[i]);
        if (dumped) save_report();
        return;
    }
}

static void report_pci(void)
{
    /* Registry snapshots only. Re-enumerating or BAR-sizing here can disable
     * decoding underneath an already active driver. Boot's BSP calls us after
     * dev_probe_all; no runtime hotplug/removal is supported by this reporter. */
    int count = dev_count();
    for (int i = 0; i < count; i++) {
        const struct device *d = dev_at(i);
        if (!d || d->bus_type != DEV_BUS_PCI) continue;
        emit("pci %04x:%02x:%02x.%u %04x:%04x class=%02x.%02x.%02x driver=%s",
             d->seg, d->bus, d->slot, d->func, d->vendor, d->device,
             d->class_code, d->subclass, d->prog_if, d->drv ? d->drv->name : "unbound");
    }
}

static void report_memory(void)
{
    /* Previously managed_mib used pmm_total_frames()/256. That is the span
     * through the highest usable frame, including reserved/MMIO holes, not
     * usable RAM. The model's 16 GiB span / 12 GiB usable fixture catches this
     * distinction. Firmware usable bytes still do not measure DIMM capacity. */
    emit("memory firmware_usable_mib=%llu source=pmm (not DIMM capacity)",
         (unsigned long long)(pmm_total_bytes() / (1024 * 1024)));
    const struct pch_spd_inventory *spd = pch_spd_inventory();
    /* The inventory pointer exists even before probing. io_base is published
     * by the SMBus probe only once its aperture is usable; a zero-initialized
     * inventory does not mean that a scan found zero DIMMs. */
    if (!spd || !spd->io_base) {
        emit("spd NOT_OBSERVED controller not probed");
        return;
    }
    emit("spd modules=%u capacity_mib=%llu rejected=%u read_errors=%u source=SPD",
         spd->modules, (unsigned long long)spd->module_capacity_mib,
         spd->rejected, spd->read_errors);
    for (unsigned i = 0; i < spd->modules && i < PCH_SPD_MAX_MODULES; i++) {
        const struct pch_spd_module *m = &spd->module[i];
        /* Address is the identity. Two DIMMs may share a firmware locator;
         * inventing bank/channel names here would collapse distinct modules. */
        emit("dimm address=%02x capacity_mib=%u part=%s ranks=%u bus_width=%u ecc_bits=%u",
             m->address, m->ddr3.module_mib, m->ddr3.part_number,
             m->ddr3.ranks, m->ddr3.bus_width, m->ddr3.ecc_bits);
    }
}

void hwreport_dump(void)
{
    emit("begin boot diagnostics; not verified on X79-VG4 hardware");
    report_pci();
    struct smp_topology_summary cpu;
    smp_topology_get_summary(&cpu);
    emit("cpu online=%d logical=%u cores=%u performance_cores=%u efficiency_cores=%u unknown_cores=%u apic_mismatches=%u class_conflicts=%u",
         smp_cpu_count(), cpu.logical, cpu.cores, cpu.performance_cores,
         cpu.efficiency_cores, cpu.unknown_cores, cpu.apic_mismatches, cpu.class_conflicts);
    report_memory();
    const struct blkdev *root = blk_root();
    if (root)
        emit("root device=%s parent=%s start_lba=%llu sectors=%llu mounted=%s",
             root->name, root->parent ? root->parent->name : "whole-disk",
             (unsigned long long)root->start, (unsigned long long)root->nsectors,
             root_ready ? "yes" : "no");
    else
        emit("root device=none mounted=no");
    for (unsigned i = 0; i < LEVELS; i++)
        emit("milestone %s %s", names[i], details[i][0] ? details[i] : "NOT_OBSERVED");
    emit("end");
    dumped = 1;
    save_report();
}
