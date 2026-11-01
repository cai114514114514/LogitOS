/* Boot-framebuffer ownership for AMD/ATI PCI display functions.
 *
 * UEFI GOP or a VGA option ROM can leave a usable linear framebuffer long
 * before LogitOS has enough of an AMDGPU stack to own display clocks, memory
 * controllers, rings, fences and reset.  The tempting shortcut is to bind any
 * 1002 display once fb.c has a Multiboot framebuffer.  That is wrong on a
 * multi-GPU machine: the framebuffer can belong to another PCI function.
 * This observer therefore binds only when the COMPLETE half-open LFB range is
 * contained by one sane memory BAR on this exact function.
 *
 * Ownership validation is deliberately read-only.  After it succeeds, the
 * exact RV100 1002:5159 path may run amd_accel's isolated fill/copy canary;
 * Polaris10 in an HWHUD=1 build (LOGIT_HWHUD) also samples BAR5 registers
 * without command submission; every other AMD GPU, and every build without
 * HWHUD, remains passive.  In particular this driver never wakes
 * D3 hardware or enables BME, DMA, IRQs, clocks, modesetting or 3D.  Desktop
 * rendering remains fb.c's CPU copy into the established aperture.
 *
 * Stage B3 (HWHUD builds only) adds one step beyond passive: the Polaris10
 * self-test (polaris/selftest/selftest.c) may take an exclusive ownership
 * lease (gpu_lease.h), write registers, load microcode and run a fill and a
 * copy inside a private 2 MiB window of the VRAM aperture.  It never installs
 * the present hook, so the desktop stays CPU-composited whatever happens, and
 * a \LOGIT\NOGPU marker on the boot stick switches it off.  dev_quiesce_all
 * calls amd_bootfb_quiesce before a reboot or S5, which checks the engine is
 * idle and ends the lease.
 */
#include <stddef.h>
#include <stdint.h>
#include "amd/bootfb.h"
#include "amd/accel.h"
#include "amd/polaris/resource/device.h"
#include "driver.h"
#include "pci.h"
#include "fb.h"
#include "kprintf.h"
#include "amd/gpu_lease.h"
#ifdef LOGIT_HWHUD
#include "amd/polaris/memory/arena.h"
#include "ktime.h"
#endif
/* The self-test is compiled in exactly when the HWHUD display survey is. The
 * watched mutation compiles the self-test glue into a plain build too, so the
 * host gate can prove its plain-build assertions are able to fail. */
#if defined(LOGIT_HWHUD) || defined(AMD_BOOTFB_NEGCTL_SELFTEST_IN_ANY_BUILD)
#define AMD_BOOTFB_SELFTEST_COMPILED 1
#include "amd/polaris/selftest/selftest.h"
#include "kheap.h"
#include "ktime.h"
#endif

#define AMD_VENDOR_ID 0x1002u
#define PCI_CAP_PM     0x01u
#define PCI_PMCSR      0x04u
#define POLARIS_REGISTER_BAR 5
#define DISPLAY_HUD_LINES 2u
#define DISPLAY_HUD_LINE_BYTES 112
#define SELFTEST_HUD_LINE_BYTES 160

static const char amd_bootfb_name[] = "AMD/ATI firmware framebuffer";
static struct device *polaris_boot_device;
static int polaris_resources_probed;
/* Which door the VBIOS came through, kept for the HUD: the serial line below
 * is gone from a machine without a serial cable. NULL until a probe ran. */
static const char *polaris_vbios_label;
#ifdef LOGIT_HWHUD
static struct polaris_survey display_survey;
/* display_survey is all zero until the survey ran, and zero reads as "arena
 * selected" (POLARIS_ARENA_OK); this flag keeps that from ever being believed. */
static int display_survey_ran;
static char display_hud_line[DISPLAY_HUD_LINES][DISPLAY_HUD_LINE_BYTES];
#endif
/* The ownership lease of stage B3. A struct in every build (a few bytes); only
 * the self-test, which exists in HWHUD builds, ever takes it. */
static struct amd_gpu_lease gpu_lease;
#ifdef AMD_BOOTFB_SELFTEST_COMPILED
static struct polaris_selftest_report selftest_report;
static char selftest_hud_line[SELFTEST_HUD_LINE_BYTES];
#endif
/* Defined with its initializer at the end of the file; the self-test asks it
 * whether the device is bound to this driver. */
static struct driver amd_bootfb_driver;

static int memory_bar_sane(const struct dev_resource *r)
{
    if (!(r->flags & DEV_RES_MEM) ||
#ifndef AMD_BOOTFB_NEGCTL_ACCEPT_MIXED_BAR
        (r->flags & DEV_RES_IO) ||
#endif
        !r->start || !r->size)
        return 0;
    return r->size <= UINT64_MAX - r->start;
}

int amd_bootfb_probe(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI ||
        dev->vendor != AMD_VENDOR_ID ||
        dev->class_code != PCI_CLASS_DISPLAY || dev->header_type != 0)
        return -1;

    uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                      PCI_CFG_COMMAND);
    if (command == UINT16_MAX) {
        kprintf("[amd-bootfb] %s %04x:%04x refused: PCI command unavailable\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }
#ifndef AMD_BOOTFB_NEGCTL_ACCEPT_MEM_OFF
    if (!(command & PCI_CMD_MEM)) {
        /* Re-enabling decode is not passive recovery: the BAR assignment or
         * firmware display state may already be gone. */
        kprintf("[amd-bootfb] %s %04x:%04x refused: PCI memory decode is off\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }
#endif

    uint8_t pm = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_PM);
    unsigned power_state = 0;
    if (pm) {
#ifndef AMD_BOOTFB_NEGCTL_ACCEPT_BAD_PM
        if (pm > 0xfbu) {
            kprintf("[amd-bootfb] %s %04x:%04x refused: malformed PM capability\n",
                    dev->name, dev->vendor, dev->device);
            return -1;
        }
#endif
        power_state = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                     (uint16_t)(pm + PCI_PMCSR)) & 3u;
        if (power_state != 0) {
            kprintf("[amd-bootfb] %s %04x:%04x refused: PCI power state D%u\n",
                    dev->name, dev->vendor, dev->device, power_state);
            return -1;
        }
    }

    uint32_t width = fb_width(), height = fb_height();
    uint64_t lfb = 0, lfb_bytes = 0;
    if (!width || !height || !fb_boot_lfb_range(&lfb, &lfb_bytes) ||
        !lfb_bytes || lfb_bytes > UINT64_MAX - lfb) {
        kprintf("[amd-bootfb] %s %04x:%04x refused: no valid Multiboot LFB\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    int owner_bar = -1;
    uint64_t lfb_end = lfb + lfb_bytes;
#ifdef AMD_BOOTFB_NEGCTL_START_ONLY
    (void)lfb_end;
#endif
    for (int i = 0; i < DEV_NRES; i++) {
        const struct dev_resource *r = &dev->res[i];
        if (!memory_bar_sane(r)) continue;
        uint64_t bar_end = r->start + r->size;
#ifdef AMD_BOOTFB_NEGCTL_START_ONLY
        /* Watched mutation: reproduces the unsafe start-address-only test. */
        if (lfb >= r->start && lfb < bar_end) {
#else
        if (lfb >= r->start && lfb_end <= bar_end) {
#endif
            owner_bar = i;
            break;
        }
    }
    if (owner_bar < 0) {
        kprintf("[amd-bootfb] %s %04x:%04x refused: LFB %p/%p is outside its BARs\n",
                dev->name, dev->vendor, dev->device,
                (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes);
        return -1;
    }

    kprintf("[amd-bootfb] %s %04x:%04x mode=passive bootfb=%ux%u "
            "source=multiboot-lfb bar=%d lfb=%p/%p cmd=%04x",
            dev->name, dev->vendor, dev->device, width, height, owner_bar,
            (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes, command);
    if (pm) kprintf(" pm=D%u", power_state);
    else    kprintf(" pm=none");
    kprintf("\n");
    int accel = amd_accel_prepare(dev, lfb, lfb_bytes, width, height);
    if (accel == 0)
        kprintf("[amd-bootfb] framebuffer retained; RV100 2D canary passed, "
                "desktop remains CPU-composited\n");
    else if (dev->device == 0x5159u)
        kprintf("[amd-bootfb] framebuffer retained; RV100 canary blocked, "
                "desktop remains CPU-composited\n");
    else if (polaris10_device_known(dev->device))
        kprintf("[amd-bootfb] framebuffer retained; Polaris read-only probe, "
                "SMU/GART/ring bring-up pending; desktop remains CPU-composited\n");
    else
        kprintf("[amd-bootfb] framebuffer retained; no modeset, GPU command, "
                "BAR map, DMA, IRQ or MMIO write on this device\n");
    dev_set_drvdata(dev, (void *)amd_bootfb_name);
    if (polaris10_device_known(dev->device)) {
        polaris_boot_device = dev;
        polaris_resources_probed = 0;
    }
    return 0;
}

#ifdef LOGIT_HWHUD
/* The scan's only register access: one 32-bit read of the mapped BAR5. */
static uint32_t read_register_bar(void *context, uint32_t byte_offset)
{
    const uint64_t *base = context;
    return *(volatile uint32_t *)(uintptr_t)(*base + byte_offset);
}

/* time_mono_ns is TSC backed once calibrated; the serial line names the
 * clocksource actually in use so a PIT fallback cannot pass as a TSC reading. */
static uint64_t monotonic_nanoseconds(void *context)
{
    (void)context;
    return time_mono_ns();
}

/* A survey that does not run must still say why, in the one place a photo of
 * the HUD can show it: the serial line and display_hud_line[0] carry the same
 * text, and line 1 is cleared so the HUD never pairs this reason with an arena
 * line left over from another device. */
static void note_survey_skipped(const char *reason)
{
    ksnprintf(display_hud_line[0], DISPLAY_HUD_LINE_BYTES, "skipped: %s", reason);
    display_hud_line[1][0] = '\0';
    kprintf("[amd-dce] %s\n", display_hud_line[0]);
}

/* Stage B2: read what the firmware left on the display and pick the VRAM arena
 * a later stage could use. Strictly read-only: BAR5 reads, a ~0.1 s frame-counter
 * measurement, and arithmetic. The arena is only evidence here; nothing owns it,
 * and BAR0 is not mapped, so the WC scanout mapping is left alone. A device whose
 * MC window could not be proven (layout_covers_vram clear) is not scanned,
 * because the MC base would be a guess. Called only in HWHUD builds: the
 * measurement stalls the boot CPU after the first desktop frame, which a normal
 * boot must not pay (docs/superpowers/plans/2026-11-01-rx580-stage-b.md, global
 * constraints: stage B is skipped in any build other than HWHUD). */
static void survey_display(struct device *device,
                           const struct polaris_resource_report *report,
                           uint64_t lfb, uint64_t lfb_bytes)
{
    uint32_t height = fb_height();
    if (report->device.result != POLARIS_PROBE_OBSERVED ||
        !report->device.layout_covers_vram || !height ||
        lfb_bytes % height) {
        note_survey_skipped("no proven MC window or framebuffer pitch");
        return;
    }
    uint64_t register_base = dev_bar_map(device, POLARIS_REGISTER_BAR);
    if (!register_base) {
        note_survey_skipped("BAR5 not mapped");
        return;
    }
    struct polaris_display_ops ops = {
        .ctx = &register_base,
        .read32 = read_register_bar,
        .now_ns = monotonic_nanoseconds
    };
    struct polaris_survey_inputs inputs = {
        .ops = &ops,
        .bar5_bytes = device->res[POLARIS_REGISTER_BAR].size,
#ifdef AMD_BOOTFB_NEGCTL_SURVEY_PITCH
        /* Watched mutation: the whole framebuffer size passed as the pitch. */
        .gop = {lfb, device->res[0].start, report->device.mc_base,
                (uint32_t)lfb_bytes},
#else
        .gop = {lfb, device->res[0].start, report->device.mc_base,
                (uint32_t)(lfb_bytes / height)},
#endif
        .lfb_bytes = lfb_bytes,
        .vbios = report->atom_result == POLARIS_ATOM_OK ?
                 &report->reservations : 0,
        .vram = {report->device.mc_base, report->device.vram_bytes},
        .aperture_bytes = report->device.cpu_aperture_bytes,
        .aperture_cpu_base = device->res[0].start,
        .firmware_bytes = report->firmware_bundle_bytes
    };
    polaris_arena_survey(&inputs, &display_survey);
    display_survey_ran = 1;

    polaris_survey_format_gop(display_hud_line[0], DISPLAY_HUD_LINE_BYTES,
                              &display_survey);
    kprintf("[amd-dce] %s clock=%s\n", display_hud_line[0],
            time_source_name(time_get_source()));
    for (unsigned index = 0; index < POLARIS_DISPLAY_CRTC_COUNT; ++index) {
        const struct polaris_crtc_state *crtc = &display_survey.scan.crtc[index];
        if (display_survey.scan_result != POLARIS_DISPLAY_OK || !crtc->active ||
            (display_survey.gop_result == POLARIS_GOP_MATCH &&
             index == display_survey.gop_crtc)) {
            continue;
        }
        char line[DISPLAY_HUD_LINE_BYTES];
        polaris_display_format_crtc(line, sizeof line, crtc, 0, 0);
        kprintf("[amd-dce] %s\n", line);
    }
    polaris_arena_format(display_hud_line[1], DISPLAY_HUD_LINE_BYTES,
                         display_survey.arena_result, &display_survey.plan);
    kprintf("[amd-arena] %s\n", display_hud_line[1]);
}

/* The HUD reads these after boot; the survey text is kept rather than rebuilt
 * so the panel cannot show anything the serial log did not. */
const char *amd_bootfb_display_hud_line(unsigned index)
{
    if (index >= DISPLAY_HUD_LINES || !display_hud_line[index][0])
        return NULL;
    return display_hud_line[index];
}
#else
/* hwhud.c is linked into every kernel, so the accessor must exist; without
 * HWHUD no survey ever ran and there is never a line to show. */
const char *amd_bootfb_display_hud_line(unsigned index)
{
    (void)index;
    return NULL;
}
#endif

#ifdef AMD_BOOTFB_SELFTEST_COMPILED
/* ---- stage B3: the GPU self-test, kernel side ------------------------------
 * polaris/selftest/selftest.c holds the logic and knows nothing about the
 * kernel; these adapters give it the device, the mappings, the lease, the
 * clock, the firmware loader and the heap. Host gates replace the whole set
 * with models (tests/hw/b3), and tests/gpu/amd/bootfb_test.c drives these very
 * adapters against stub kernel services. */

#define POLARIS_APERTURE_BAR 0
/* The adapters return this when a kernel service gave nothing back. */
#define SELFTEST_ENVIRONMENT_FAILED 1

/* Why the NOGPU marker is read through the boot-module registry: see
 * polaris/selftest/opt_out.c. */
static int selftest_opted_out(void *context)
{
    (void)context;
    return polaris_selftest_opted_out();
}

/* Why the binding is part of the lease: the device model hands a device to one
 * driver only, so "bound to amd-bootfb" is how "no other driver may bind" is
 * known to hold (c/drivers/core/driver.h). */
static int selftest_acquire_lease(void *context)
{
    struct device *device = context;
    int bound = device->drv == &amd_bootfb_driver &&
                dev_get_drvdata(device) == (void *)amd_bootfb_name;
    int result = amd_gpu_lease_acquire(&gpu_lease, device, bound);
    if (result) {
        kprintf("[amd-polaris-runtime] lease refused: %s\n",
                amd_gpu_lease_result_name(result));
    }
    return result;
}

/* Asked before every register access, so it reads the lease itself and keeps
 * no copy. */
static int selftest_lease_held(void *context)
{
    return amd_gpu_lease_holds(&gpu_lease, context);
}

/* Ends the lease for good at quiesce; see amd_gpu_lease_retire. */
static void selftest_retire_lease(void *context)
{
    amd_gpu_lease_retire(&gpu_lease, context);
}

/* Why dev_bar_map for BAR5 but a window for BAR0: BAR5 holds only registers and
 * nothing else maps it; BAR0 holds the live scanout, whose write-combining
 * mapping a whole-BAR map would overwrite (dev_bar_map_window in
 * c/drivers/core/device.c explains). Both mappings are identity mappings, so
 * the returned physical address is also the CPU pointer. */
static int selftest_map_registers(void *context, volatile uint8_t **bar5)
{
    uint64_t base = dev_bar_map(context, POLARIS_REGISTER_BAR);
    if (!base) {
        return SELFTEST_ENVIRONMENT_FAILED;
    }
    *bar5 = (volatile uint8_t *)(uintptr_t)base;
    return 0;
}

/* The only BAR0 mapping the self-test ever asks for: the arena window, at its
 * planned offset. dev_bar_map_window maps whole pages, and the arena sits
 * megabytes away from the scanout, so no page is shared with it. */
static int selftest_map_arena_window(void *context, uint64_t bar_offset,
                                     uint64_t bytes, volatile uint8_t **window)
{
    uint64_t base = dev_bar_map_window(context, POLARIS_APERTURE_BAR, bar_offset, bytes);
    if (!base) {
        return SELFTEST_ENVIRONMENT_FAILED;
    }
    *window = (volatile uint8_t *)(uintptr_t)base;
    return 0;
}

/* The native platform re-reads configuration space before every access. */
static int selftest_read_pci_config(void *context, uint16_t byte_offset, uint32_t *value)
{
    const struct device *device = context;
    *value = pci_cfg_read(device->bus, device->slot, device->func, byte_offset);
    return 0;
}

/* A read-only look at BAR5 for the bind-failure line. The mapping is the one the
 * self-test already holds (dev_bar_map is idempotent); the offset is checked
 * against the BAR's size before it is dereferenced. */
static int selftest_read_diagnostic_register(void *context, uint32_t byte_offset,
                                             uint32_t *value)
{
    struct device *device = context;
    const struct dev_resource *registers = &device->res[POLARIS_REGISTER_BAR];
    if ((byte_offset & 3u) || registers->size < 4u || byte_offset > registers->size - 4u) {
        return SELFTEST_ENVIRONMENT_FAILED;
    }
    uint64_t base = dev_bar_map(device, POLARIS_REGISTER_BAR);
    if (!base) {
        return SELFTEST_ENVIRONMENT_FAILED;
    }
    *value = *(volatile uint32_t *)(uintptr_t)(base + byte_offset);
    return 0;
}

/* The self-test times its waits in microseconds; the kernel's monotonic clock
 * is TSC backed once calibrated (ktime.h). */
static uint64_t selftest_now_us(void *context)
{
    (void)context;
    return time_mono_ns() / 1000u;
}

/* Loads exactly the file set the key selects, with the pin and ownership checks
 * of resource/files.c; the probe at boot only tried both candidate sets. */
static int selftest_load_firmware(void *context, unsigned security_key,
                                  struct polaris_resource_files *files)
{
    const struct device *device = context;
    return polaris_resource_files_load(device->device, device->revision,
                                       security_key, files);
}

/* The runtime copies what it needs onto the card, so the host copies can go. */
static void selftest_release_firmware(void *context, struct polaris_resource_files *files)
{
    (void)context;
    polaris_resource_files_release(files);
}

/* The firmware bundle is assembled in ordinary RAM first, then written to the
 * card with explicit 32-bit stores (runtime.c). */
static void *selftest_allocate_workspace(void *context, size_t bytes)
{
    (void)context;
    return kmalloc(bytes);
}

/* Pairs with selftest_allocate_workspace. */
static void selftest_free_workspace(void *context, void *workspace)
{
    (void)context;
    kfree(workspace);
}

/* Static storage on purpose: the platform the runtime keeps points back here. */
static struct polaris_selftest_ops selftest_ops = {
    .opted_out = selftest_opted_out,
    .acquire_lease = selftest_acquire_lease,
    .lease_held = selftest_lease_held,
    .retire_lease = selftest_retire_lease,
    .map_registers = selftest_map_registers,
    .map_arena_window = selftest_map_arena_window,
    .read_pci_config = selftest_read_pci_config,
    .read_diagnostic_register = selftest_read_diagnostic_register,
    .now_us = selftest_now_us,
    .load_firmware = selftest_load_firmware,
    .release_firmware = selftest_release_firmware,
    .allocate_workspace = selftest_allocate_workspace,
    .free_workspace = selftest_free_workspace
};

/* Run the self-test on the probed Polaris device and keep its HUD text. The
 * arena plan comes from the display survey; a survey that did not run or did
 * not choose an arena hands the self-test NULL, and its gate then says why
 * nothing was attempted. The inputs outlive the call (static) because the
 * runtime's memory request points into the plan. */
static void run_selftest(struct device *device,
                         const struct polaris_resource_report *report,
                         uint64_t lfb_bytes)
{
    static struct polaris_selftest_inputs inputs;
    uint32_t height = fb_height();
    inputs = (struct polaris_selftest_inputs){
        .pci_vendor = device->vendor,
        .pci_device = device->device,
        .pci_revision = device->revision,
        .vram = {report->device.mc_base, report->device.vram_bytes},
        .bar0_physical = device->res[POLARIS_APERTURE_BAR].start,
        .bar5_physical = device->res[POLARIS_REGISTER_BAR].start,
        .bar5_bytes = device->res[POLARIS_REGISTER_BAR].size,
        .width = fb_width(),
        .height = height,
        .pitch = height ? (uint32_t)(lfb_bytes / height) : 0,
        .firmware_bundle_bytes = report->firmware_bundle_bytes
    };
#ifdef LOGIT_HWHUD
    if (display_survey_ran && display_survey.arena_result == POLARIS_ARENA_OK) {
        inputs.arena_plan = &display_survey.plan;
    }
#endif
    selftest_ops.context = device;
    polaris_selftest_run(&inputs, &selftest_ops, &selftest_report);
    polaris_selftest_format_hud(selftest_hud_line, sizeof selftest_hud_line,
                                &selftest_report);
}

/* The HUD reads this after boot; like the survey lines it is the serial text
 * rebuilt from the same report, never separately worded. */
const char *amd_bootfb_gpu_runtime_hud_line(void)
{
    return selftest_hud_line[0] ? selftest_hud_line : NULL;
}
#else
/* hwhud.c is linked into every kernel, so the accessor must exist; without the
 * self-test there is never a line to show. */
const char *amd_bootfb_gpu_runtime_hud_line(void)
{
    return NULL;
}
#endif

void amd_bootfb_probe_resources(void)
{
    if (!polaris_boot_device || polaris_resources_probed)
        return;
    uint64_t lfb, bytes;
    if (!fb_boot_lfb_range(&lfb, &bytes))
        return;

    /* Keep VFS resource loading in the boot phase that guarantees both root
     * mount and completed driver binding. Boot is still single-threaded, so
     * the device cannot be unplugged during these reads. The resource layer
     * rechecks PCI/BAR identity before accessing the device. */
    polaris_resources_probed = 1;
    struct polaris_resource_report report;
    int status = polaris_resources_probe_device(polaris_boot_device, lfb, bytes,
                                                fb_width(), fb_height(), &report);
    kprintf("[amd-resources] pci=%04x status=%d vbios-bytes=%u atom=%d "
            "atom-result=%s usage-revision=%u "
            "firmware-key0=%x firmware-key1=%x required=%x "
            "ownership-missing=%u security-key-unknown=%u "
            "vbios-source=%s vbios-module=%s vbios-vfct=%s "
            "firmware-sources=rootfs:%u,bootmod:%u pinned=%s\n",
            polaris_boot_device->device, status, report.vbios_bytes,
            report.atom_result, polaris_atom_result_name(report.atom_result),
            report.reservations.table_revision,
            report.firmware_mask[0], report.firmware_mask[1], POLARIS_RESOURCE_ALL_FILES,
            report.ownership_missing, report.security_key_unknown,
            polaris_vbios_source_name(report.vbios_source),
            polaris_vbios_status_name(report.vbios_module_status),
            polaris_vbios_status_name(report.vbios_vfct_status),
            report.firmware_from_rootfs, report.firmware_from_bootmod,
            report.firmware_pin_mismatch ? "mismatch" : "ok");
    /* A refused probe never looked for a VBIOS, so "none" would be a claim. */
    if (status == 0)
        polaris_vbios_label = polaris_vbios_source_name(report.vbios_source);
    if (report.atom_result == POLARIS_ATOM_OK) {
        kprintf("[amd-resources] firmware-reserved=%llx/%llu driver-scratch=%llx/%llu\n",
                (unsigned long long)report.reservations.firmware.base,
                (unsigned long long)report.reservations.firmware.bytes,
                (unsigned long long)report.reservations.driver_scratch.base,
                (unsigned long long)report.reservations.driver_scratch.bytes);
    }
#ifdef LOGIT_HWHUD
    if (!status)
        survey_display(polaris_boot_device, &report, lfb, bytes);
#endif
#ifdef AMD_BOOTFB_SELFTEST_COMPILED
    if (!status)
        run_selftest(polaris_boot_device, &report, bytes);
#endif
}

const char *amd_bootfb_vbios_source(const struct device *dev)
{
    return dev && dev == polaris_boot_device ? polaris_vbios_label : NULL;
}

/* Why a callback: dev_quiesce_all leaves display devices bound (they show the
 * shutdown), so it cannot use ->remove to stop a GPU that the self-test had
 * working. This is the hook it calls instead. Without a lease it does nothing
 * and prints nothing, so a machine whose GPU was never touched, and QEMU, see
 * no change. Returns nonzero when the GPU could not be confirmed idle. */
static int amd_bootfb_quiesce(struct device *dev)
{
#ifdef AMD_BOOTFB_SELFTEST_COMPILED
    if (dev != polaris_boot_device) {
        return 0;
    }
    int unconfirmed = polaris_selftest_quiesce(&selftest_ops, &selftest_report);
    if (selftest_report.result != POLARIS_SELFTEST_NOT_RUN) {
        polaris_selftest_format_hud(selftest_hud_line, sizeof selftest_hud_line,
                                    &selftest_report);
    }
    return unconfirmed;
#else
    (void)dev;
    return 0;
#endif
}

static void amd_bootfb_remove(struct device *dev)
{
    if (dev == polaris_boot_device) {
        if (amd_gpu_lease_holds(&gpu_lease, dev)) {
            /* The card is in a state only this boot's driver understood; the
             * lease ends for good rather than letting a rebind trust it. */
            kprintf("[amd-polaris-runtime] device unbound while leased; "
                    "GPU is not touched again this boot\n");
        }
        amd_gpu_lease_retire(&gpu_lease, dev);
        polaris_boot_device = NULL;
        polaris_resources_probed = 0;
        polaris_vbios_label = NULL;
#ifdef LOGIT_HWHUD
        display_hud_line[0][0] = '\0';
        display_hud_line[1][0] = '\0';
        display_survey_ran = 0;
#endif
    }
    amd_accel_reset();
    if (dev) dev_set_drvdata(dev, NULL);
}

static const struct dev_match amd_bootfb_ids[] = {
    { AMD_VENDOR_ID, DEV_ANY, PCI_CLASS_DISPLAY, DEV_ANYC, DEV_ANYC, 0 },
    DEV_MATCH_END
};

#ifdef LOGIT_HOST_TEST
const struct dev_match *amd_bootfb_match_table(void)
{
    return amd_bootfb_ids;
}
#endif

static struct driver amd_bootfb_driver = {
    .name = "amd-bootfb",
    .bus_type = DEV_BUS_PCI,
    .match = amd_bootfb_ids,
    .probe = amd_bootfb_probe,
    .remove = amd_bootfb_remove,
    .quiesce = amd_bootfb_quiesce,
};
DRIVER_DECLARE(amd_bootfb_driver);

#ifdef LOGIT_HOST_TEST
const struct driver *amd_bootfb_driver_for_test(void)
{
    return &amd_bootfb_driver;
}
#endif
