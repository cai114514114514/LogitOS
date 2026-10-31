/* IRQ wiring is a per-device lifecycle operation: request/release run in
 * the binding callback owner (or the single BSP bootstrap), never an ISR.
 * dev_unbind first wins unbinding and drains bind_busy before release/remove.
 * No module-wide lock is needed around BAR mapping or IRQ retirement. */
/* MSI / MSI-X, and the legacy INTx fallback, behind one model-level call.
 *
 * Why this is not per driver: an MSI is nothing but "write this data to this
 * address when you want attention", where the address encodes a LAPIC and the
 * data encodes a vector. Every driver that hand-rolls it ends up hand-picking a
 * vector, hand-installing an IDT gate and hand-routing a GSI -- which is exactly
 * why the e1000 in this tree is polled: there was no way to ask for an
 * interrupt. dev_irq_request() is that way.
 *
 * Preference order MSI-X > MSI > INTx. MSI-X first because its table lives in a
 * BAR (so it needs no config-space writes per vector and supports far more of
 * them); MSI second; INTx last because it is shared, level-triggered and needs
 * the I/O APIC.
 *
 * Message address/data (Intel SDM 10.11):
 *   address = 0xFEE00000 | (destination APIC id << 12)
 *             bit 3 = redirection hint (0 = fixed), bit 2 = destination mode
 *             (0 = physical). Both left at 0.
 *   data    = vector | (delivery mode << 8) | (level << 14) | (trigger << 15)
 *             Delivery mode 000 = fixed, edge-triggered. MSI is edge by
 *             definition -- it is a posted write, there is no line to level. */
#include <stdint.h>
#include <stddef.h>
#include "pci.h"
#include "pci_route.h"
#include "pci_platform.h"
#include "driver.h"
#include "irq.h"
#include "vmm.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi.h"
#include "smp.h"
#include "kprintf.h"
#include "io_lock.h"

#define MSI_ADDR_BASE   0xFEE00000u

/* MSI message-control bits (config cap + 2) */
#define MSI_MC_ENABLE   0x0001
#define MSI_MC_MMC      0x000E      /* multiple-message capable (log2) */
#define MSI_MC_MME      0x0070      /* multiple-message enable (log2) */
#define MSI_MC_64BIT    0x0080
#define MSI_MC_PVM      0x0100      /* per-vector masking */

/* MSI-X message-control bits (config cap + 2) */
#define MSIX_MC_TSIZE   0x07FF      /* table size - 1 */
#define MSIX_MC_FUNCMASK 0x4000
#define MSIX_MC_ENABLE  0x8000

static int msi_addr(uint32_t apic_id, uint32_t *address)
{
    /* FFH is the all-APIC broadcast value in the legacy 8-bit physical
     * destination format, so the last usable unicast ID is FEH. */
    if (!address || apic_id >= 0xffu) {
        kprintf("[irq] refusing MSI 8-bit destination apic_id=%u\n",
                (unsigned)apic_id);
        return -1;
    }
    *address = MSI_ADDR_BASE | (apic_id << 12);
    return 0;
}
static uint32_t msi_data(int vec)          { return (uint32_t)(vec & 0xFF); }
static int intx_source(struct device *d, int enable);
static int msix_teardown(struct device *d);
static int msi_teardown(struct device *d);

#ifdef LOGIT_HOST_TEST
/* Hosted fault fixtures replace this weak stop with a longjmp recorder. */
__attribute__((weak, noreturn))
void pci_msi_host_fail_stop(struct device *d, const char *source)
{
    (void)d;
    (void)source;
    __builtin_trap();
}
#endif

/* A message source that stays live after a failed quiesce can target a vector
 * whose callback owner is about to be freed.  There is no safe error return in
 * that state, so stop instead of manufacturing a use-after-free. */
static __attribute__((noreturn)) void irq_source_fail_stop(struct device *d,
                                                           const char *source)
{
    kprintf("[irq] %s: %s source could not be quiesced; stopping\n",
            d ? d->name : "?", source);
#ifdef LOGIT_HOST_TEST
    pci_msi_host_fail_stop(d, source);
#else
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
#endif
}

/* --------------------------------------------------------------- MSI-X -- */
static int msix_setup(struct device *d, int vec)
{
    uint32_t address;
    if (msi_addr(lapic_id(), &address) != 0) return -1;
    uint8_t c = d->cap_msix;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
    uint32_t tbl = pci_cfg_read(d->bus, d->slot, d->func, (uint16_t)(c + 4));
    int bir = (int)(tbl & 7);
    uint64_t off = tbl & ~7u;

    if (bir >= DEV_NRES) return -1;
    uint64_t base = dev_bar_map(d, bir);
    if (!base) return -1;
    if (off + 16 > d->res[bir].size) return -1;      /* table outside its own BAR */

    /* First make any firmware-left MSI-X state provably quiet.  Programming a
     * target while ENABLE=1/FUNCMASK=0 could send a half-written message. */
    if (msix_teardown(d) != 0) irq_source_fail_stop(d, "MSI-X");
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_ALTERNATE_MESSAGE_QUIESCE
    if (d->cap_msi && msi_teardown(d) != 0)
        irq_source_fail_stop(d, "alternate MSI");
#endif
    if (intx_source(d, 0) != 0) return -1;
    uint16_t after;

    volatile uint32_t *e = (volatile uint32_t *)(uintptr_t)(base + off);
    e[3] = 1;                                        /* vector control: masked */
    __asm__ volatile ("" ::: "memory");
    if (!(e[3] & 1u)) return -1;
    e[0] = address;
    e[1] = 0;
    e[2] = msi_data(vec);
    __asm__ volatile ("" ::: "memory");
    if (e[0] != address || e[1] != 0 || e[2] != msi_data(vec)) return -1;
    e[3] = 0;                                        /* unmask this vector */
    __asm__ volatile ("" ::: "memory");
    if (e[3] & 1u) return -1;

    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)((mc & ~MSIX_MC_FUNCMASK) | MSIX_MC_ENABLE));
    after = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_MSIX_SETUP_READBACK
    if ((after & (MSIX_MC_ENABLE | MSIX_MC_FUNCMASK)) != MSIX_MC_ENABLE) {
        int quiet = msix_teardown(d);
#ifdef LOGIT_X2APIC_NEGCTL_TRUST_STALE_SETUP_STATE
        if (quiet != 0 && (after & MSIX_MC_ENABLE) &&
            !(after & MSIX_MC_FUNCMASK))
#else
        /* `after` may itself be a stale/posted read. Only teardown's fresh
         * write+readback can prove the source quiet enough for fallback. */
        if (quiet != 0)
#endif
            irq_source_fail_stop(d, "MSI-X");
        return -1;
    }
#else
    (void)after;
#endif
    if (intx_source(d, 0) != 0) {
        if (msix_teardown(d) != 0) irq_source_fail_stop(d, "MSI-X");
        return -1;
    }
    return 0;
}

static int msix_teardown(struct device *d)
{
    uint8_t c = d->cap_msix;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)((mc | MSIX_MC_FUNCMASK) & ~MSIX_MC_ENABLE));
    uint16_t after = pci_cfg_read16(d->bus, d->slot, d->func,
                                    (uint16_t)(c + 2));
#ifdef LOGIT_X2APIC_NEGCTL_SKIP_MSIX_TEARDOWN_READBACK
    (void)after;
    return 0;
#else
    return (after & (MSIX_MC_ENABLE | MSIX_MC_FUNCMASK)) ==
           MSIX_MC_FUNCMASK ? 0 : -1;
#endif
}

/* ----------------------------------------------------------------- MSI -- */
static int msi_setup(struct device *d, int vec)
{
    uint32_t address;
    if (msi_addr(lapic_id(), &address) != 0) return -1;
    uint8_t c = d->cap_msi;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));

    /* MSI has no function-wide mask on devices without PVM.  Clear ENABLE and
     * verify it before changing the address/data target. */
    if (msi_teardown(d) != 0) irq_source_fail_stop(d, "MSI");
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_ALTERNATE_MESSAGE_QUIESCE
    if (d->cap_msix && msix_teardown(d) != 0)
        irq_source_fail_stop(d, "alternate MSI-X");
#endif
    if (intx_source(d, 0) != 0) return -1;
    uint16_t after;

    pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 4), address);
    if (mc & MSI_MC_64BIT) {
        pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 8), 0);
        pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 12), (uint16_t)msi_data(vec));
        if (mc & MSI_MC_PVM)                          /* mask bits at cap+0x10 */
            pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 16), 0);
    } else {
        pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 8), (uint16_t)msi_data(vec));
        if (mc & MSI_MC_PVM)                          /* mask bits at cap+0x0C */
            pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 12), 0);
    }
    if (pci_cfg_read(d->bus, d->slot, d->func, (uint16_t)(c + 4)) != address)
        return -1;
    if (mc & MSI_MC_64BIT) {
        if (pci_cfg_read(d->bus, d->slot, d->func, (uint16_t)(c + 8)) != 0 ||
            pci_cfg_read16(d->bus, d->slot, d->func,
                           (uint16_t)(c + 12)) != (uint16_t)msi_data(vec))
            return -1;
        if ((mc & MSI_MC_PVM) &&
            (pci_cfg_read(d->bus, d->slot, d->func, (uint16_t)(c + 16)) & 1u))
            return -1;
    } else {
        if (pci_cfg_read16(d->bus, d->slot, d->func,
                           (uint16_t)(c + 8)) != (uint16_t)msi_data(vec))
            return -1;
        if ((mc & MSI_MC_PVM) &&
            (pci_cfg_read(d->bus, d->slot, d->func, (uint16_t)(c + 12)) & 1u))
            return -1;
    }

    /* One vector: MME = 0 regardless of how many the device offers. */
    mc = (uint16_t)((mc & ~MSI_MC_MME) | MSI_MC_ENABLE);
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2), mc);
    after = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_MSI_SETUP_READBACK
    if ((after & (MSI_MC_ENABLE | MSI_MC_MME)) != MSI_MC_ENABLE) {
        int quiet = msi_teardown(d);
#ifdef LOGIT_X2APIC_NEGCTL_TRUST_STALE_SETUP_STATE
        if (quiet != 0 && (after & MSI_MC_ENABLE))
#else
        /* As above, retirement is decided by teardown's new readback, never
         * by the earlier setup snapshot that reported the mismatch. */
        if (quiet != 0)
#endif
            irq_source_fail_stop(d, "MSI");
        return -1;
    }
#else
    (void)after;
#endif
    if (intx_source(d, 0) != 0) {
        if (msi_teardown(d) != 0) irq_source_fail_stop(d, "MSI");
        return -1;
    }
    return 0;
}

static int msi_teardown(struct device *d)
{
    uint8_t c = d->cap_msi;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)(mc & ~MSI_MC_ENABLE));
    uint16_t after = pci_cfg_read16(d->bus, d->slot, d->func,
                                    (uint16_t)(c + 2));
#ifdef LOGIT_X2APIC_NEGCTL_SKIP_MSI_TEARDOWN_READBACK
    (void)after;
    return 0;
#else
    return (after & MSI_MC_ENABLE) ? -1 : 0;
#endif
}

/* ---------------------------------------------------------------- INTx -- */
#define INTX_MAX_LINES IRQ_NVEC
#define INTX_MAX_MEMBERS 64 /* matches the bounded device registry */
struct intx_line {
    uint32_t gsi;
    int used, vector;
    int retiring; /* 1 draining, 2 quarantined after failed hardware masking */
    unsigned members;
    int level, active_low;
    int source; /* Highest-priority electrical description accepted on this wire. */
};
struct intx_member {
    struct device *dev;
    struct intx_line *line;
    irq_handler_t fn;
    void *arg;
    unsigned active;
    uint64_t generation;
    uint64_t deliveries;
    struct pci_irq_route route;
    /* route describes the actual RTE. Retain the member's own request too:
     * a peer changing the wire does not mean this member has adopted AML. */
    int requested_level, requested_low;
    int retiring;
};
static struct intx_line intx_lines[INTX_MAX_LINES];
static struct intx_member intx_members[INTX_MAX_MEMBERS];
/* Member slots and vector numbers are reusable. A boot-wide sequence prevents
 * either reuse from identifying a new binding as the old delivery epoch. */
static uint64_t intx_generation;

/* For the hardware panel: one INTx line, its IOAPIC entry as read back, and
 * each member's PCI Command/Status (Status bit 3: the device is asserting). */
int pci_intx_diag(int i, char *buf, int max)
{
    int seen = 0;
    for (unsigned l = 0; l < INTX_MAX_LINES; l++) {
        struct intx_line *line = &intx_lines[l];
        if (!line->used || seen++ != i) continue;
        int n = ksnprintf(buf, max, "intx gsi %u vec %x rte %08x:", line->gsi, line->vector,
                          ioapic_rte_low(line->gsi));
        for (unsigned m = 0; m < INTX_MAX_MEMBERS && n < max; m++) {
            struct intx_member *mb = &intx_members[m];
            if (mb->line != line || !mb->dev) continue;
            struct device *d = mb->dev;
            uint16_t cmd = pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
            uint16_t sts = pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND + 2);
            n += ksnprintf(buf + n, max - n, " %02x:%02x.%u cmd %04x sts %04x%s", d->bus, d->slot,
                           d->func, cmd, sts, (sts & 8) ? " ASSERTING" : "");
        }
        return 0;
    }
    return -1;
}
static io_lock_t intx_gate = IO_LOCK_INIT;

/* One physical wire has one vector. Calling every registered device's handler
 * is mandatory: PCI INTx has no sender ID, and each driver acknowledges its
 * own status only. Re-routing the GSI for a second device, as the old code
 * did, lost the first handler and turned an uncleared shared source into a
 * level-triggered interrupt storm on physical hardware.
 *
 * Borrow each member separately, with no registry lock across the callback.
 * A device being removed cannot be called after its final active reference;
 * other members remain serviceable while that removal waits on another CPU. */
static void intx_dispatch(void *arg)
{
    struct intx_line *line = arg;
    for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
        struct intx_member *m = &intx_members[i];
        uint64_t flags = io_lock_enter(&intx_gate);
        irq_handler_t fn = NULL;
        void *data = NULL;
        if (m->line == line && m->fn && !m->retiring) {
            fn = m->fn; data = m->arg;
            m->deliveries++;
            __atomic_fetch_add(&m->active, 1, __ATOMIC_RELAXED);
        }
        io_lock_leave(&intx_gate, flags);
        if (!fn) continue;
        fn(data);
        __atomic_fetch_sub(&m->active, 1, __ATOMIC_RELEASE);
#ifdef PCI_INTX_NEGCTL_FIRST_ONLY
        break;
#endif
    }
}

static int intx_source(struct device *d, int enable)
{
    uint16_t command = pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    command = enable ? (uint16_t)(command & ~PCI_CMD_INTX_DIS) :
                       (uint16_t)(command | PCI_CMD_INTX_DIS);
    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND, command);
    /* This read is both the PCI configuration ordering barrier and ownership
     * proof.  A posted/ignored disable cannot be followed by callback removal,
     * and a failed enable cannot be reported as a working interrupt. */
    uint16_t after = pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
#ifdef LOGIT_X2APIC_NEGCTL_SKIP_INTX_SOURCE_READBACK
    (void)after;
    return 0;
#else
    if (!!(after & PCI_CMD_INTX_DIS) != !enable) {
        kprintf("[irq] %s: INTx source %s readback failed\n", d->name,
                enable ? "enable" : "disable");
        return -1;
    }
    return 0;
#endif
}

/* Intel ICH9 / 6 Series / C600 / 7 Series: a bus-0 PCH device's INTx pin
 * reaches the IOAPIC through PIRQA-H at GSI 16-23, chosen per device by the
 * DxxIR registers in the chipset configuration space (RCBA, LPC config 0xF0).
 * Offsets and the GSI 16 base: coreboot src/southbridge/intel/bd82x6x/pch.h
 * and src/southbridge/intel/common/rcba_pirq.c. The firmware interrupt line
 * (config 0x3C) is the 8259 routing; it matches the IOAPIC only where the
 * chipset mirrors ISA IRQs onto pins 0-15, as QEMU's ICH9 does. On the
 * X79-VG4 it named pins nothing drives (2026-10-31).
 * Correction (2026-11-01): that last sentence was an inference from missing
 * interrupts, not measured wiring. Neither RCBA nor _PRT has been verified
 * on X79-VG4 hardware; retain the disagreement instead of declaring a winner
 * from model evidence. Firmware _PRT takes precedence by policy below. */
static int pch_intx_gsi(const struct device *d, struct pci_irq_route *out)
{
#ifdef PCI_NEGCTL_IGNORE_PCH_ROUTE
    (void)d;
    (void)out;
    return -1;
#else
    static const struct { uint8_t slot; uint16_t reg; } dir[] = {
        {20, 0x3160}, {22, 0x315c}, {25, 0x3150}, {26, 0x314c}, {27, 0x3148},
        {28, 0x3146}, {29, 0x3144}, {30, 0x3142}, {31, 0x3140},
    };
    if (d->bus != 0 || !d->irq_pin || d->irq_pin > 4) return -1;
    uint16_t reg = 0;
    for (unsigned i = 0; i < sizeof dir / sizeof dir[0]; i++)
        if (dir[i].slot == d->slot) reg = dir[i].reg;
    if (!reg) return -1;
    uint32_t id = pci_cfg_read(0, 31, 0, 0);
    uint16_t dev = (uint16_t)(id >> 16);
    if ((id & 0xffff) != 0x8086 ||
        !((dev >= 0x2910 && dev <= 0x2919) || (dev >= 0x1c40 && dev <= 0x1c5f) ||
          (dev >= 0x1d40 && dev <= 0x1d41) || (dev >= 0x1e40 && dev <= 0x1e5f)))
        return -1;
    uint32_t rcba = pci_cfg_read(0, 31, 0, 0xf0);
    if (!(rcba & 1) || !(rcba & 0xffffc000u)) return -1;
    uint64_t page = (uint64_t)(rcba & 0xffffc000u) + 0x3000;
    vmm_map_page(page, page, VMM_WRITABLE | VMM_NOCACHE);
    uint16_t route = *(volatile uint16_t *)(uintptr_t)(page + (reg & 0xfff));
    unsigned pirq = (route >> (4 * (d->irq_pin - 1))) & 7;
    *out = (struct pci_irq_route){
        .gsi = 16 + pirq, .level = 1, .active_low = 1,
        .source = PCI_ROUTE_RCBA, .pin = d->irq_pin - 1
    };
    ksnprintf(out->how, sizeof out->how, "PCH D%uIR %04x", d->slot, route);
    return 0;
#endif
}

static pci_route_resolver_fn aml_resolver;
static uint32_t intx_destination;
static unsigned intx_destination_ready;

/* Publication is independent of the callback lock: evaluating AML can sleep.
 * Only pci_prt_aml_ready installs this resolver, after _PIC and namespace init. */
void pci_intx_set_aml_resolver(pci_route_resolver_fn fn)
{
    __atomic_store_n(&aml_resolver, fn, __ATOMIC_RELEASE);
}

/* Keep spelling authoritative for serial output and the hardware panel. */
const char *pci_route_source_name(int source)
{
    switch (source) {
    case PCI_ROUTE_AML:
        return "_PRT";
    case PCI_ROUTE_RCBA:
        return "RCBA";
    case PCI_ROUTE_FIRMWARE:
        return "firmware";
    default:
        return "none";
    }
}

/* Prefer evaluated firmware topology, then PCH routing, then the BIOS line.
 * Compute RCBA even on an AML hit: disagreement is essential board evidence.
 * The old comment claimed the X79 pins were undriven; neither RCBA nor _PRT
 * wiring has been verified on X79-VG4 hardware. Retain both values in logs. */
static int intx_route_for(struct device *device, struct pci_irq_route *route)
{
    if (!device->irq_pin || device->irq_pin > 4) {
        return -1;
    }
    struct pci_irq_route pch;
    int have_pch = pch_intx_gsi(device, &pch) == 0;
    pci_route_resolver_fn resolver = __atomic_load_n(&aml_resolver, __ATOMIC_ACQUIRE);
#ifdef PRT_NEGCTL_PREFER_RCBA
    if (have_pch) {
        *route = pch;
        return 0;
    }
#endif
    if (resolver && resolver(device, route) == 0) {
        if (have_pch && pch.gsi != route->gsi) {
            kprintf("[irq-route] %s CONFLICT _PRT gsi %u vs RCBA gsi %u (using _PRT)\n",
                    device->name, route->gsi, pch.gsi);
        }
        return 0;
    }
    if (have_pch) {
        *route = pch;
        return 0;
    }
    if (!device->irq_line || device->irq_line == 0xff) {
        return -1;
    }
    *route = (struct pci_irq_route){
        .gsi = device->irq_line < 16 ? acpi_gsi_for_irq(device->irq_line) : device->irq_line,
        .level = 1, .active_low = 1, .source = PCI_ROUTE_FIRMWARE,
        .pin = device->irq_pin - 1
    };
    ksnprintf(route->how, sizeof route->how, "fw line %u", device->irq_line);
    return 0;
}

/* Establish membership using the existing source-disable, verified RTE and
 * vector quarantine lifecycle. Always retain the original bootstrap target:
 * the AML worker can run on an AP which never serviced the device at probe. */
static int intx_bind_route(struct device *device, irq_handler_t fn, void *arg,
                           const struct pci_irq_route *route)
{
    uint32_t gsi = route->gsi;
    uint64_t target_flags = io_lock_enter(&intx_gate);
    if (!intx_destination_ready) {
        intx_destination = lapic_id();
        intx_destination_ready = 1;
    }
    uint32_t destination = intx_destination;
    io_lock_leave(&intx_gate, target_flags);
    if (!ioapic_can_route(gsi, destination)) {
        return -1;
    }
    /* Firmware may have left INTx enabled. Suppress this source until its
     * complete shared-line membership and verified IOAPIC route are live. */
    if (intx_source(device, 0) != 0) {
        return -1;
    }
    for (;;) {
        uint64_t flags = io_lock_enter(&intx_gate);
        struct intx_line *line = NULL, *empty = NULL;
        struct intx_member *member = NULL;
        for (unsigned i = 0; i < INTX_MAX_LINES; i++) {
            if (intx_lines[i].used && intx_lines[i].gsi == gsi) {
                line = &intx_lines[i];
            }
            if (!intx_lines[i].used && !empty) {
                empty = &intx_lines[i];
            }
        }
        if (line && line->retiring) {
            int quarantined = line->retiring == 2;
            io_lock_leave(&intx_gate, flags);
            if (quarantined) {
                return -1;
            }
            /* The final owner drains outside this lock. */
            io_relax();
            continue;
        }
        for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
            if (!intx_members[i].dev) {
                member = &intx_members[i];
                break;
            }
        }
        if (!member || (!line && !empty)) {
            io_lock_leave(&intx_gate, flags);
            return -1;
        }
        int first = !line;
        if (first) {
            line = empty;
            int vec = irq_alloc_vector(intx_dispatch, line, "pci-intx");
            if (vec < 0) {
                io_lock_leave(&intx_gate, flags);
                return -1;
            }
            *line = (struct intx_line){ .gsi = gsi, .used = 1, .vector = vec,
                .level = route->level && pci_intx_level(), .active_low = route->active_low };
        }
        int level = route->level && pci_intx_level();
        int active_low = route->active_low;
        int source = route->source;
        /* One RTE has one electrical mode (Intel 82093AA section 3.2.4).
         * Correction: the last joining member previously overwrote it, so a
         * firmware-low peer could silently undo an AML-high route. Apply the
         * same _PRT > RCBA > firmware policy at the WIRE, not just per device.
         * Retain the best accepted description until the wire is retired:
         * removing its AML member does not change physical board wiring. */
#ifndef PRT_NEGCTL_SHARED_LAST_WINS
        if (!first && line->source < source) {
            level = line->level;
            active_low = line->active_low;
            source = line->source;
        }
#endif
        int changed = line->level != level || line->active_low != active_low;
        if (!first && changed && source == line->source) {
            /* Contradictory descriptions at equal authority cannot both be
             * true. Refuse the newcomer while keeping existing peers live;
             * allow a sole AML owner to update a wire shared with fallbacks. */
            int conflict = 0;
            for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
                struct intx_member *peer = &intx_members[i];
                if (peer->line == line && peer->dev && !peer->retiring &&
                    peer->route.source == source &&
                    (peer->requested_level != level || peer->requested_low != active_low)) {
                    conflict = 1;
                }
            }
            if (conflict) {
                kprintf("[irq-route] %s shared GSI %u electrical CONFLICT via=%s; polling required\n",
                        device->name, gsi, pci_route_source_name(source));
                io_lock_leave(&intx_gate, flags);
                return -1;
            }
        }
        *member = (struct intx_member){ .dev = device, .line = line, .fn = fn,
            .arg = arg, .route = *route, .generation = ++intx_generation,
            .requested_level = route->level && pci_intx_level(),
            .requested_low = route->active_low };
        line->members++;
        /* Install the complete dispatcher/member before enabling a source. An
         * existing shared line must retain its vector and remote-IRR state. */
        int route_status = (first || changed) ?
            ioapic_route(gsi, (uint8_t)line->vector, destination,
                         level, active_low) : IOAPIC_ROUTE_OK;
        /* _PRT may change polarity on a shared GSI (q35 GSIA-H are high).
         * Preserve its vector and other members, but verify the new electrical
         * mode. On a safe reject restore their old RTE before returning. */
        if (!first && route_status != IOAPIC_ROUTE_OK) {
            *member = (struct intx_member){0};
            line->members--;
            int restored = ioapic_route(gsi, (uint8_t)line->vector, destination,
                                         line->level, line->active_low);
            if (restored != IOAPIC_ROUTE_OK) {
                /* Retain the dispatcher and its remaining members. Even a
                 * failed mask may still target this vector, so it must never
                 * be recycled. Drivers' polling paths can continue; halting
                 * here would turn a silent wire into a machine-wide outage. */
                line->retiring = 2;
                kprintf("[irq-route] GSI %u shared route quarantined; polling required\n", gsi);
                io_lock_leave(&intx_gate, flags);
                return -1;
            }
            io_lock_leave(&intx_gate, flags);
            return -1;
        }
        if (route_status != IOAPIC_ROUTE_OK) {
            int failed_vec = line->vector;
            *member = (struct intx_member){0};
#ifndef LOGIT_X2APIC_NEGCTL_FREE_UNSAFE_INTX
            if (route_status == IOAPIC_ROUTE_UNSAFE) {
                /* The RTE may still target failed_vec. Keep both its line and
                 * vector permanently quarantined; recycling either could send
                 * a stale interrupt into an unrelated new callback. */
                line->members = 0;
                line->retiring = 2;
                io_lock_leave(&intx_gate, flags);
                return -1;
            }
#endif
            *line = (struct intx_line){0};
            io_lock_leave(&intx_gate, flags);
            irq_free_vector(failed_vec);
            return -1;
        }
        if (intx_source(device, 1) != 0) {
            int failed_vec = line->vector;
            *member = (struct intx_member){0};
            line->members--;
            if (!first) {
                /* The RTE may already have changed polarity for this joining
                 * member. If its source enable fails, restore the remaining
                 * peers' old electrical mode before rollback can rejoin.
                 * Otherwise metadata says low while hardware stays high. */
                if (changed && ioapic_route(gsi, (uint8_t)line->vector, destination,
                                             line->level, line->active_low) != IOAPIC_ROUTE_OK) {
                    /* Same quarantine rule as a rejected route update:
                     * remaining callbacks and their vector still have owners. */
                    line->retiring = 2;
                    kprintf("[irq-route] GSI %u shared route quarantined; polling required\n", gsi);
                    io_lock_leave(&intx_gate, flags);
                    return -1;
                }
                io_lock_leave(&intx_gate, flags);
                return -1;
            }
            /* A failed enable read back as disabled, but the first route is
             * already live.  Recycle its vector only after masking the RTE is
             * confirmed; otherwise quarantine the stale destination. */
            if (ioapic_mask(gsi) != 0) {
                line->retiring = 2;
                io_lock_leave(&intx_gate, flags);
                return -1;
            }
            *line = (struct intx_line){0};
            io_lock_leave(&intx_gate, flags);
            irq_free_vector(failed_vec);
            return -1;
        }
        line->level = level;
        line->active_low = active_low;
        line->source = source;
        /* Publish only after route AND source readback succeeded. Otherwise a
         * failed move's restore must still see every old peer's old mode.
         * Keep device-specific provenance, but make route_of describe hardware. */
        for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
            struct intx_member *peer = &intx_members[i];
            if (peer->line == line && peer->dev && !peer->retiring) {
                peer->route.level = level;
                peer->route.active_low = active_low;
            }
        }
        int vec = line->vector;
#ifndef PRT_NEGCTL_REROUTE_STALE_VEC
        /* Publish vector and epoch under the same gate that accounts ISR
         * delivery. A snapshot must never pair a new epoch with an old vector.
         * Dispatch cannot borrow this enabled source until we drop the gate. */
        __atomic_store_n(&device->irq_vec, (int16_t)vec, __ATOMIC_RELEASE);
#endif
        io_lock_leave(&intx_gate, flags);
        return vec;
    }
}

/* Route selection is separate from binding so rollback can restore the exact
 * old GSI instead of asking the now-changed AML resolver a second time. */
static int intx_setup(struct device *device, irq_handler_t fn, void *arg)
{
    struct pci_irq_route route;
    if (intx_route_for(device, &route) != 0) {
        return -1;
    }
    int vector = intx_bind_route(device, fn, arg, &route);
    if (vector >= 0) {
        pci_intx_route_of(device, &route);
        kprintf("[irq-route] %s INT%c gsi %u %s %s via=%s (%s) fw-line %u\n",
                device->name, 'A' + device->irq_pin - 1, route.gsi,
                route.level && pci_intx_level() ? "level" : "edge",
                route.active_low ? "low" : "high", pci_route_source_name(route.source),
                route.how, device->irq_line);
    }
    return vector;
}

/* Suppress a source and retire its last wire only after the IOAPIC has also
 * drained the old level delivery. Return 1 for a quarantined vector: normal
 * teardown can finish, but a reroute must leave this device polled, not rebind
 * it after an unacknowledged EOI. Negative returns leave ownership intact. */
static int intx_release(struct device *d)
{
    /* PCI source suppression comes first even for a non-last member. Masking
     * the shared GSI here would stop every other device on the wire. */
    if (intx_source(d, 0) != 0) return -1;
    uint64_t flags = io_lock_enter(&intx_gate);
    struct intx_member *member = NULL;
    for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++)
        if (intx_members[i].dev == d) { member = &intx_members[i]; break; }
    if (!member) { io_lock_leave(&intx_gate, flags); return -1; }
    struct intx_line *line = member->line;
    member->retiring = 1;
    member->fn = NULL; /* no new dispatcher can borrow this callback */
    int last = --line->members == 0;
    if (last) line->retiring = 1; /* blocks a new first owner until vector drain */
    io_lock_leave(&intx_gate, flags);

    int masked = 0;
    if (last) {
#ifndef PCI_INTX_NEGCTL_SKIP_MASK
        masked = ioapic_mask(line->gsi);
#endif
    }
#ifndef PCI_INTX_NEGCTL_NO_MEMBER_DRAIN
    while (__atomic_load_n(&member->active, __ATOMIC_ACQUIRE)) io_relax();
#endif
    if (last) {
        if (masked) {
            /* Mask alone used to count as retirement. Linux v6.12 io_apic.c
             * ioapic_irq_get_chip_state() also requires Remote IRR to clear:
             * queued LAPIC work need not be an active callback yet. Keep the
             * vector reserved if either masking or that EOI boundary fails. */
            flags = io_lock_enter(&intx_gate);
            *member = (struct intx_member){0};
            line->retiring = 2;
            io_lock_leave(&intx_gate, flags);
            kprintf("[irq] GSI %u mask failed; vector %d quarantined\n", line->gsi, line->vector);
            return 1;
        }
        /* irq_free_vector closes dispatcher admission, drains callbacks/EOI
         * and quarantines CPU/vector pairs until CPU-local LAPIC IRR is clear
         * (Linux v6.12 vector.c __vector_cleanup()). ioapic_mask's explicit
         * EOI alone cannot prove that an old CPU delivery has completed.
         * Callback arguments can be released now; a late vector is counted
         * and acknowledged without touching this line. Never hold intx_gate. */
        irq_free_vector(line->vector);
    }
    flags = io_lock_enter(&intx_gate);
    *member = (struct intx_member){0};
    if (last) *line = (struct intx_line){0};
    io_lock_leave(&intx_gate, flags);
    return 0;
}

/* Copy live metadata under the callback gate; never evaluate AML here. */
int pci_intx_route_of(const struct device *device, struct pci_irq_route *out)
{
    if (!device || !out) {
        return -1;
    }
    IO_GUARD(&intx_gate);
    for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
        struct intx_member *member = &intx_members[i];
        if (member->dev == device && !member->retiring) {
            *out = member->route;
            return 0;
        }
    }
    return -1;
}

/* Count at dispatcher admission, not after the driver callback: reroute's
 * synthetic catchup must never manufacture physical delivery evidence. The
 * gate also prevents mixing fields while release+bind reuses the same vector. */
int pci_intx_delivery_snapshot(const struct device *device, struct pci_intx_delivery *out)
{
    if (!device || !out) {
        return -1;
    }
    IO_GUARD(&intx_gate);
    for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
        const struct intx_member *member = &intx_members[i];
        if (member->dev == device && !member->retiring) {
            out->generation = member->generation;
            out->vector = member->line->vector;
            out->count = member->deliveries;
            return 0;
        }
    }
    return -1;
}

/* Serialize only reroute passes, without a kernel mutex symbol dependency:
 * pci_msi.c is also linked by freestanding standalone PCI tests. This atomic
 * mutex leaves IF unchanged; it is never held by ISR code. Device lifetime is
 * separately borrowed through bind_busy, exactly like dev_unbind's contract. */
static unsigned reroute_owner;
static void (*reroute_wait)(void);

/* Keep the scheduler out of this standalone-linkable PCI unit. The AML
 * consumer supplies a sleeping worker, never a timer/ISR callback. */
void pci_intx_set_reroute_wait(void (*wait)(void))
{
    __atomic_store_n(&reroute_wait, wait, __ATOMIC_RELEASE);
}

/* Move each entry from a device snapshot: reusing an earlier free member slot
 * during a split must not cause that device to be processed twice.
 * Correction: bind_busy was once treated like unbinding. EHCI's short report
 * publication can own it, so that silently stranded a live fallback route
 * forever after AML's one-shot ready(). Retain only contended entries and
 * retry from the sleeping AML worker, with no routing lock held during sleep.
 * 100 waits requesting 5 ms each cap the retry count, with 500 ms of requested
 * delay. Timer granularity/scheduling can extend it; this contention policy
 * is not a hardware response-time assertion. */
void pci_intx_reroute_all(struct pci_reroute_stats *out)
{
    struct pci_reroute_stats stats = {0};
    void (*wait)(void) = __atomic_load_n(&reroute_wait, __ATOMIC_ACQUIRE);
    unsigned waits = 0;
    enum { RETRY_LIMIT = 100 };
    while (__atomic_exchange_n(&reroute_owner, 1, __ATOMIC_ACQUIRE)) {
        io_relax();
    }
    struct device *devices[INTX_MAX_MEMBERS];
    uint64_t generations[INTX_MAX_MEMBERS];
    unsigned count = 0;
    uint64_t flags = io_lock_enter(&intx_gate);
    for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
        if (intx_members[i].dev && !intx_members[i].retiring) {
            devices[count] = intx_members[i].dev;
            generations[count] = intx_members[i].generation;
            count++;
        }
    }
    io_lock_leave(&intx_gate, flags);
    stats.devices = count;
    for (;;) {
        unsigned pending = 0;
        for (unsigned i = 0; i < count; i++) {
            struct device *device = devices[i];
            if (!device) {
                continue;
            }
            unsigned idle = 0;
            if (__atomic_load_n(&device->unbinding, __ATOMIC_ACQUIRE)) {
                devices[i] = NULL;
                stats.kept++;
                continue;
            }
            if (!__atomic_compare_exchange_n(&device->bind_busy, &idle, 1, 0,
                                              __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
#ifndef PRT_NEGCTL_REROUTE_NO_RETRY
                if (wait && waits < RETRY_LIMIT) {
                    pending++;
                    continue;
                }
#endif
                devices[i] = NULL;
                stats.kept++;
                struct pci_irq_route retained;
                if (pci_intx_route_of(device, &retained) == 0) {
                    kprintf("[irq-route] %s INT%c gsi %u kept via=%s (route busy; %s)\n",
                            device->name, 'A' + device->irq_pin - 1, retained.gsi,
                            pci_route_source_name(retained.source),
                            wait ? "retry limit reached" : "no retry worker");
                }
                continue;
            }
            devices[i] = NULL;
            struct pci_irq_route old, route;
            int requested_level = 0, requested_low = 0;
            irq_handler_t fn = NULL;
            void *arg = NULL;
            flags = io_lock_enter(&intx_gate);
            for (unsigned m = 0; m < INTX_MAX_MEMBERS; m++) {
                struct intx_member *member = &intx_members[m];
                if (member->dev == device && !member->retiring &&
                    member->generation == generations[i]) {
                    /* A sleeping retry must not borrow a replacement binding:
                     * dev_unbind may finish and the same registry slot rebind. */
                    old = member->route;
                    requested_level = member->requested_level;
                    requested_low = member->requested_low;
                    fn = member->fn;
                    arg = member->arg;
                    break;
                }
            }
            io_lock_leave(&intx_gate, flags);
            if (!fn || __atomic_load_n(&device->unbinding, __ATOMIC_ACQUIRE)) {
                stats.kept++;
                goto release_owner;
            }
            pci_route_resolver_fn resolver = __atomic_load_n(&aml_resolver, __ATOMIC_ACQUIRE);
            /* Absence of _PRT is not a request to discard the working fallback. */
            if (!resolver || resolver(device, &route) != 0) {
                stats.kept++;
                kprintf("[irq-route] %s INT%c gsi %u kept via=%s (no _PRT entry)\n",
                        device->name, 'A' + device->irq_pin - 1, old.gsi, pci_route_source_name(old.source));
                goto release_owner;
            }
            struct pci_irq_route pch;
            if (pch_intx_gsi(device, &pch) == 0 && pch.gsi != route.gsi) {
                kprintf("[irq-route] %s CONFLICT _PRT gsi %u vs RCBA gsi %u (using _PRT)\n",
                        device->name, route.gsi, pch.gsi);
            }
            int level = route.level && pci_intx_level();
            if (old.gsi == route.gsi && old.level == level &&
                old.active_low == route.active_low && requested_level == level &&
                requested_low == route.active_low) {
                flags = io_lock_enter(&intx_gate);
                for (unsigned m = 0; m < INTX_MAX_MEMBERS; m++) {
                    if (intx_members[m].dev == device) {
                        intx_members[m].route = route;
                        intx_members[m].route.level = level;
                        /* Matching electrical bits still confirm firmware
                         * authority. Without this promotion a later conflicting
                         * peer could treat this AML wire as a fallback wire. */
                        if (route.source < intx_members[m].line->source) {
                            intx_members[m].line->source = route.source;
                        }
                    }
                }
                io_lock_leave(&intx_gate, flags);
                stats.same++;
                kprintf("[irq-route] %s INT%c gsi %u same via=_PRT\n",
                        device->name, 'A' + device->irq_pin - 1, route.gsi);
                goto release_owner;
            }
            int released = intx_release(device);
            if (released < 0) {
                stats.failed++;
                kprintf("[irq-route] %s FAILED gsi %u release refused\n", device->name, old.gsi);
                goto release_owner;
            }
            if (released > 0) {
                /* The old GSI is isolated and its vector is still reserved. Do
                 * not attempt either the new route or rollback into that pin;
                 * driver polling remains the only safe completion path. */
                __atomic_store_n(&device->irq_vec, -1, __ATOMIC_RELEASE);
                device->irq_mode = DEV_IRQ_NONE;
                stats.lost++;
                kprintf("[irq-route] %s LOST (INTx disabled)\n", device->name);
                kprintf("[irq-route] %s GSI %u isolated; INTx disabled, polling required\n",
                        device->name, old.gsi);
                goto release_owner;
            }
            int vector = intx_bind_route(device, fn, arg, &route);
            if (vector >= 0) {
                stats.moved++;
                /* The probe-time [irq] line remains unchanged for existing gates.
                 * Retirement may allocate a different vector even on the same
                 * GSI; log it here so later delivery evidence has a live owner. */
                kprintf("[irq-route] %s INT%c gsi %u -> %u moved via=_PRT (was %s) vector %d\n",
                        device->name, 'A' + device->irq_pin - 1, old.gsi, route.gsi,
                        pci_route_source_name(old.source), vector);
#ifndef PRT_NEGCTL_REROUTE_NO_CATCHUP
                /* Level sources retain status while INTX_DIS is set. The catchup
                 * also covers emulated edges and chipset latches. bind_busy keeps
                 * the driver's callback alive; intx_gate is not held here. */
                fn(arg);
#endif
            } else {
#ifndef PRT_NEGCTL_REROUTE_NO_ROLLBACK
                vector = intx_bind_route(device, fn, arg, &old);
#endif
                if (vector >= 0) {
                    stats.failed++;
                    /* Rollback rebuilds the route, so its vector may change too. */
                    kprintf("[irq-route] %s FAILED gsi %u restored vector %d\n",
                            device->name, old.gsi, vector);
                } else {
                    __atomic_store_n(&device->irq_vec, -1, __ATOMIC_RELEASE);
                    device->irq_mode = DEV_IRQ_NONE;
                    stats.lost++;
                    kprintf("[irq-route] %s LOST (INTx disabled)\n", device->name);
                }
            }
release_owner:
            __atomic_store_n(&device->bind_busy, 0, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&reroute_owner, 0, __ATOMIC_RELEASE);
        if (!pending) {
            break;
        }
        waits++;
        wait();
        while (__atomic_exchange_n(&reroute_owner, 1, __ATOMIC_ACQUIRE)) {
            io_relax();
        }
    }
    if (out) {
        *out = stats;
    }
}

/* Per-member hardware evidence: Intel 82093AA section 3.2.4 gives RIRR bit
 * 14, Delivery Status bit 12 and Mask bit 16. PCI 3.0 Status bit 3 reports
 * assertion, not CPU delivery; retain both to diagnose a silent physical wire. */
int pci_route_diag(int index, char *buffer, int max)
{
    if (!buffer || max <= 0 || index < 0) {
        return -1;
    }
    IO_GUARD(&intx_gate);
    for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++) {
        struct intx_member *member = &intx_members[i];
        if (!member->dev || member->retiring || index-- != 0) {
            continue;
        }
        struct device *device = member->dev;
        uint32_t rte = ioapic_rte_low(member->route.gsi);
        uint16_t status = pci_cfg_read16(device->bus, device->slot, device->func, PCI_CFG_COMMAND + 2);
        ksnprintf(buffer, max,
                  "route %02x:%02x.%u %c gsi %u vec %x via %s rte %08x RIRR %u DS %u M %u n %llu ASSERT %u",
                  device->bus, device->slot, device->func, 'A' + device->irq_pin - 1, member->route.gsi,
                  member->line->vector, pci_route_source_name(member->route.source), rte,
                  (rte >> 14) & 1, (rte >> 12) & 1, (rte >> 16) & 1,
                  (unsigned long long)dev_irq_count(device), (status >> 3) & 1);
        return 0;
    }
    return -1;
}

/* --------------------------------------------------------- the model API -- */
static int g_max_mode = DEV_IRQ_MSIX;

#ifndef LOGIT_X2APIC_NEGCTL_SKIP_ALTERNATE_MESSAGE_QUIESCE
static void message_sources_quiesce(struct device *dev)
{
    if (dev->cap_msix && msix_teardown(dev) != 0)
        irq_source_fail_stop(dev, "preflight MSI-X");
    if (dev->cap_msi && msi_teardown(dev) != 0)
        irq_source_fail_stop(dev, "preflight MSI");
}
#endif

int dev_irq_prefer(int max_mode)
{
    if (max_mode >= DEV_IRQ_INTX && max_mode <= DEV_IRQ_MSIX)
        return __atomic_exchange_n(&g_max_mode, max_mode, __ATOMIC_ACQ_REL);
    return __atomic_load_n(&g_max_mode, __ATOMIC_ACQUIRE);
}

int dev_irq_request(struct device *dev, irq_handler_t fn, void *arg, const char *name)
{
    if (!dev || !fn || dev->bus_type != DEV_BUS_PCI) return -1;
    if (dev->irq_mode != DEV_IRQ_NONE) return dev->irq_vec;   /* already wired */

    /* This preflight is above every early return and fallback: no LAPIC, a wide
     * APIC ID, an invalid MSI-X BAR, or a forced INTx policy must never leave a
     * firmware-enabled message source running beside the selected path. */
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_ALTERNATE_MESSAGE_QUIESCE
    message_sources_quiesce(dev);
#endif

    /* Message-signalled interrupts are delivered by a memory write to the LAPIC,
     * so there must be a LAPIC to write to. Before smp_init() there is not. */
    int have_lapic = lapic_ready();
    int max_mode = __atomic_load_n(&g_max_mode, __ATOMIC_ACQUIRE);

    int vec = -1;
    if (have_lapic && ((max_mode >= DEV_IRQ_MSIX && dev->cap_msix) ||
                      (max_mode >= DEV_IRQ_MSI && dev->cap_msi)))
        vec = irq_alloc_vector(fn, arg, name);

    if (vec >= 0 && max_mode >= DEV_IRQ_MSIX && dev->cap_msix && msix_setup(dev, vec) == 0) {
        dev->irq_mode = DEV_IRQ_MSIX;
    } else if (vec >= 0 && max_mode >= DEV_IRQ_MSI && dev->cap_msi && msi_setup(dev, vec) == 0) {
        dev->irq_mode = DEV_IRQ_MSI;
    } else {
        if (vec >= 0) irq_free_vector(vec);
        vec = intx_setup(dev, fn, arg);
        if (vec < 0) return -1;
        dev->irq_mode = DEV_IRQ_INTX;
    }
    dev->irq_vec = (int16_t)vec;
    kprintf("[irq] %s: %s vector %d (%s)\n", dev->name,
            dev->irq_mode == DEV_IRQ_MSIX ? "msi-x" :
            dev->irq_mode == DEV_IRQ_MSI  ? "msi" : "intx",
            vec, name ? name : "?");
    return vec;
}

int dev_irq_release(struct device *dev)
{
    if (!dev || dev->irq_mode == DEV_IRQ_NONE) return 0;
    if (dev->irq_mode == DEV_IRQ_INTX) {
        if (intx_release(dev) < 0) return -1;
        dev->irq_mode = DEV_IRQ_NONE; dev->irq_vec = -1;
        return 0;
    }
    if (dev->irq_mode == DEV_IRQ_MSIX) {
        if (msix_teardown(dev) != 0) return -1;
    } else if (dev->irq_mode == DEV_IRQ_MSI) {
        if (msi_teardown(dev) != 0) return -1;
    }
    if (dev->irq_vec >= 0) irq_free_vector(dev->irq_vec);
    dev->irq_mode = DEV_IRQ_NONE;
    dev->irq_vec  = -1;
    return 0;
}

uint64_t dev_irq_count(const struct device *dev)
{
    if (!dev || dev->irq_vec < 0) return 0;
    return irq_vector_count(dev->irq_vec);
}
