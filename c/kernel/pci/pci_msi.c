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
};
struct intx_member {
    struct device *dev;
    struct intx_line *line;
    irq_handler_t fn;
    void *arg;
    unsigned active;
    int retiring;
};
static struct intx_line intx_lines[INTX_MAX_LINES];
static struct intx_member intx_members[INTX_MAX_MEMBERS];
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

static int intx_setup(struct device *d, irq_handler_t fn, void *arg)
{
    if (!d->irq_pin || d->irq_pin > 4 || !d->irq_line || d->irq_line == 0xff) return -1;
    /* Without an AML interpreter we cannot evaluate _PRT, so the GSI comes from
     * the firmware-programmed interrupt line (config 0x3C) -- which is what the
     * e1000 has always used here. Lines < 16 go through the MADT's ISA source
     * overrides; other values are GSIs. This is still firmware-line fallback,
     * not an ACPI _PRT interpreter or PCI bridge interrupt swizzling. */
    uint32_t gsi = (d->irq_line < 16) ? acpi_gsi_for_irq(d->irq_line) : d->irq_line;
    uint32_t destination = lapic_id();
    if (!ioapic_can_route(gsi, destination)) return -1;
    /* Firmware may have left INTx enabled. Suppress this source until its
     * complete shared-line membership and verified IOAPIC route are live. */
    if (intx_source(d, 0) != 0) return -1;
    for (;;) {
        uint64_t flags = io_lock_enter(&intx_gate);
        struct intx_line *line = NULL, *empty = NULL;
        struct intx_member *member = NULL;
        for (unsigned i = 0; i < INTX_MAX_LINES; i++) {
            if (intx_lines[i].used && intx_lines[i].gsi == gsi) line = &intx_lines[i];
            if (!intx_lines[i].used && !empty) empty = &intx_lines[i];
        }
        if (line && line->retiring) {
            int quarantined = line->retiring == 2;
            io_lock_leave(&intx_gate, flags);
            if (quarantined) return -1;
            io_relax(); continue; /* final owner drains outside this lock */
        }
        for (unsigned i = 0; i < INTX_MAX_MEMBERS; i++)
            if (!intx_members[i].dev) { member = &intx_members[i]; break; }
        if (!member || (!line && !empty)) {
            io_lock_leave(&intx_gate, flags); return -1;
        }
        int first = !line;
        if (first) {
            line = empty;
            int vec = irq_alloc_vector(intx_dispatch, line, "pci-intx");
            if (vec < 0) { io_lock_leave(&intx_gate, flags); return -1; }
            *line = (struct intx_line){ .gsi = gsi, .used = 1, .vector = vec };
        }
        *member = (struct intx_member){ .dev=d, .line=line, .fn=fn, .arg=arg };
        line->members++;
        /* Install the complete dispatcher/member before enabling a source. An
         * existing shared line must retain its vector and remote-IRR state. */
        int route_status = first ?
            ioapic_route(gsi, (uint8_t)line->vector, destination,
                         pci_intx_level(), 1) : IOAPIC_ROUTE_OK;
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
        if (intx_source(d, 1) != 0) {
            int failed_vec = line->vector;
            *member = (struct intx_member){0};
            line->members--;
            if (!first) {
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
        int vec = line->vector;
        io_lock_leave(&intx_gate, flags);
        return vec;
    }
}

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
            /* Keep the vector and static line state quarantined if masking was
             * not acknowledged. Freeing it would allow hardware to interrupt
             * a new driver's callback through the stale route. */
            flags = io_lock_enter(&intx_gate);
            *member = (struct intx_member){0};
            line->retiring = 2;
            io_lock_leave(&intx_gate, flags);
            kprintf("[irq] GSI %u mask failed; vector %d quarantined\n", line->gsi, line->vector);
            return 0;
        }
        /* This existing retirement primitive closes dispatcher admission and
         * drains the entire old ISR callback, including another member whose
         * concurrent removal is still waiting. Never hold intx_gate here. */
        irq_free_vector(line->vector);
    }
    flags = io_lock_enter(&intx_gate);
    *member = (struct intx_member){0};
    if (last) *line = (struct intx_line){0};
    io_lock_leave(&intx_gate, flags);
    return 0;
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
        if (intx_release(dev) != 0) return -1;
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
