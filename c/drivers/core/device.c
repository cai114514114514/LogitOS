/* The device model: a registry of enumerated devices, a list of drivers with
 * match tables, and the probe/bind pass that joins them.
 *
 * Shape notes (the "why" a reader cannot reconstruct from the code):
 *   - The registry is a fixed array, not a heap list. Enumeration runs before
 *     the kernel heap is interesting and the count on any machine we target is
 *     tens, not thousands; a fixed array also means dev_at(i) pointers stay
 *     valid for the life of the system, which is what lets a driver stash a
 *     `struct device *` in its own state.
 *   - Binding is first-driver-wins in registration order, and a driver that
 *     returns non-zero from probe() leaves the device free. That is what makes
 *     a generic class-code driver (say "any AHCI") and a specific
 *     vendor:device quirk driver able to coexist: register the specific one
 *     first.
 *   - There is no unbind-on-failure bookkeeping beyond clearing dev->drv,
 *     because probe() failure is a boot-time event on this system, not a
 *     runtime one. */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "pci.h"
#include "vmm.h"
#include "kprintf.h"
#include "io_lock.h"
#if __STDC_HOSTED__
#include <sched.h>
#else
#include "sched.h"
#endif

#define DEV_MAX 64

static struct device  g_devs[DEV_MAX];
static int            g_ndev;
static struct driver *g_drivers;        /* singly linked, registration order */
static struct driver *g_drivers_tail;
/* Registry links and complete device-slot publication only. No allocator,
 * probe/remove callback, interrupt drain or console output runs under it. */
static io_lock_t registry_lock = IO_LOCK_INIT;

static struct driver *driver_next(struct driver *after)
{
    IO_GUARD(&registry_lock);
    return after ? after->next : g_drivers;
}

/* A callback owns only this device. Contending probe passes skip it; teardown
 * waits with no registry/IRQ lock, so callbacks can sleep or register drivers. */
static int dev_binding_try(struct device *d)
{
    unsigned idle = 0;
    return __atomic_compare_exchange_n(&d->bind_busy, &idle, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void dev_binding_put(struct device *d)
{ __atomic_store_n(&d->bind_busy, 0, __ATOMIC_RELEASE); }
static void dev_binding_wait(void)
{
#if __STDC_HOSTED__
    sched_yield();
#else
    sched_poll_wait();
#endif
}

int            dev_count(void)   { return __atomic_load_n(&g_ndev, __ATOMIC_ACQUIRE); }
struct device *dev_at(int i)     { return (i >= 0 && i < dev_count()) ? &g_devs[i] : NULL; }

struct device *dev_add(const struct device *proto)
{
    IO_GUARD(&registry_lock);
    int index = dev_count();
    if (index >= DEV_MAX) return NULL;
    struct device *d = &g_devs[index];
    const uint8_t *s = (const uint8_t *)proto;
    uint8_t *t = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof *d; i++) t[i] = s[i];
    d->drv = NULL; d->match = NULL; d->drvdata = NULL;
    d->bind_busy = d->unbinding = 0;
    __atomic_store_n(&g_ndev, index + 1, __ATOMIC_RELEASE);
    return d;
}

/* ------------------------------------------------------------- iteration -- */
static int idx_after(struct device *from)
{
    if (!from) return 0;
    int i = (int)(from - g_devs);
    return (i < 0 || i >= dev_count()) ? dev_count() : i + 1;
}

struct device *dev_find_class(uint8_t class_code, uint8_t subclass, struct device *from)
{
    for (int i = idx_after(from); i < dev_count(); i++)
        if (g_devs[i].class_code == class_code &&
            (subclass == DEV_ANYC || g_devs[i].subclass == subclass))
            return &g_devs[i];
    return NULL;
}

struct device *dev_find_id(uint16_t vendor, uint16_t device, struct device *from)
{
    for (int i = idx_after(from); i < dev_count(); i++)
        if (g_devs[i].vendor == vendor && g_devs[i].device == device)
            return &g_devs[i];
    return NULL;
}

/* ----------------------------------------------------------- match tables -- */
int dev_match_one(const struct dev_match *m, const struct device *dev)
{
    if (!m || !dev) return 0;
    /* The all-zero terminator never matches anything. */
    if (!m->vendor && !m->device && !m->class_code && !m->subclass && !m->prog_if)
        return 0;
    if (m->vendor     != DEV_ANY  && m->vendor     != dev->vendor)     return 0;
    if (m->device     != DEV_ANY  && m->device     != dev->device)     return 0;
    if (m->class_code != DEV_ANYC && m->class_code != dev->class_code) return 0;
    if (m->subclass   != DEV_ANYC && m->subclass   != dev->subclass)   return 0;
    if (m->prog_if    != DEV_ANYC && m->prog_if    != dev->prog_if)    return 0;
    return 1;
}

static int match_is_end(const struct dev_match *m)
{
    return !m->vendor && !m->device && !m->class_code && !m->subclass &&
           !m->prog_if && !m->data;
}

const struct dev_match *dev_match_table(const struct dev_match *tbl, const struct device *dev)
{
    if (!tbl || !dev) return NULL;
    for (const struct dev_match *m = tbl; !match_is_end(m); m++)
        if (dev_match_one(m, dev)) return m;
    return NULL;
}

/* --------------------------------------------------------------- drivers -- */
void driver_register(struct driver *drv)
{
    if (!drv) return;
    IO_GUARD(&registry_lock);
    if (drv->next) return;                         /* already registered */
    for (struct driver *p = g_drivers; p; p = p->next)
        if (p == drv) return;
    drv->next = NULL;
    if (g_drivers_tail) g_drivers_tail->next = drv;
    else                g_drivers = drv;
    g_drivers_tail = drv;
}

#ifndef LOGIT_HOST_TEST
/* linker.ld brackets the `logit_drivers` section with these. Every
 * DRIVER_DECLARE'd driver lands here, so adding a driver file is the whole of
 * adding a driver. */
extern struct driver *__start_logit_drivers[];
extern struct driver *__stop_logit_drivers[];

static void driver_register_static(void)
{
    for (struct driver **p = __start_logit_drivers; p < __stop_logit_drivers; p++)
        driver_register(*p);
}
#else
static void driver_register_static(void) { }
#endif

int dev_probe_all(void)
{
    driver_register_static();
    int bound = 0;
    for (int i = 0; i < dev_count(); i++) {
        struct device *d = &g_devs[i];
        if (__atomic_load_n(&d->unbinding, __ATOMIC_ACQUIRE) || !dev_binding_try(d)) continue;
        if (__atomic_load_n(&d->unbinding, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&d->drv, __ATOMIC_ACQUIRE)) { dev_binding_put(d); continue; }
        for (struct driver *drv = driver_next(NULL); drv; drv = driver_next(drv)) {
            if (drv->bus_type && drv->bus_type != d->bus_type) continue;
            const struct dev_match *m = dev_match_table(drv->match, d);
            if (!m) continue;
            d->match = m;                         /* callback's match->data */
            if (drv->probe && drv->probe(d) == 0) {
                __atomic_store_n(&d->drv, drv, __ATOMIC_RELEASE);
                bound++; break;
            }
            d->match = NULL; d->drvdata = NULL;
        }
        dev_binding_put(d);
    }
    return bound;
}

void dev_unbind(struct device *dev)
{
    if (!dev) return;
    unsigned idle = 0;
    if (!__atomic_compare_exchange_n(&dev->unbinding, &idle, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        while (__atomic_load_n(&dev->unbinding, __ATOMIC_ACQUIRE)) dev_binding_wait();
        return;
    }
    while (!dev_binding_try(dev)) dev_binding_wait();
    const struct driver *drv = __atomic_load_n(&dev->drv, __ATOMIC_ACQUIRE);
    if (drv) {
        /* The probe has completed; IRQ dispatch drains before remove frees
         * callback objects. Other devices can still probe or stop meanwhile. */
        if (dev_irq_release(dev) != 0) {
            /* Releasing driver memory while an unconfirmed IRQ source can
             * still call it is a use-after-free.  Keep the complete binding
             * intact so a later unbind can retry the hardware transition. */
#ifndef LOGIT_X2APIC_NEGCTL_UNBIND_AFTER_IRQ_RELEASE_FAIL
            dev_binding_put(dev);
            __atomic_store_n(&dev->unbinding, 0, __ATOMIC_RELEASE);
            return;
#endif
        }
        if (drv->remove) drv->remove(dev);
        dev->match = NULL; dev->drvdata = NULL;
        __atomic_store_n(&dev->drv, NULL, __ATOMIC_RELEASE);
    }
    dev_binding_put(dev);
    __atomic_store_n(&dev->unbinding, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------- resources -- */
/* A PCI config write is not proof that the function accepted it.  Physical
 * firmware, bridges and a disappearing function can all leave Command
 * unchanged (or return all ones).  Keep the readback in one production helper
 * so every driver can refuse BAR access or DMA publication on the same fact.
 * This only proves the config bits; it says nothing about controller reset or
 * DMA ownership, which remains each driver's responsibility. */
static int dev_command_write_checked(struct device *dev, uint16_t wanted)
{
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, wanted);
    return pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND) == wanted
           ? 0 : -1;
}

int dev_enable_checked(struct device *dev, int bus_master)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI) return -1;
    uint16_t decode = 0;
    for (int i = 0; i < DEV_NRES; i++) {
        if (!dev->res[i].start || !dev->res[i].size) continue;
        if (dev->res[i].flags & DEV_RES_IO) decode |= PCI_CMD_IO;
        if (dev->res[i].flags & DEV_RES_MEM) decode |= PCI_CMD_MEM;
    }
    if (!decode) {
        kprintf("[device] PCI %02x:%02x.%u has no decoded BAR resource\n",
                dev->bus, dev->slot, dev->func);
        return -1;
    }
    uint16_t old = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    if (old == UINT16_MAX) {
        kprintf("[device] PCI %02x:%02x.%u Command unavailable\n",
                dev->bus, dev->slot, dev->func);
        return -1;
    }
    /* A function without I/O BARs may hardwire PCI_CMD_IO to zero (and vice
     * versa). Requiring an address space it cannot decode would reject valid
     * pure-MMIO hardware, so only request enumerated BAR types. */
    uint16_t wanted = (uint16_t)(old | decode);
    if (bus_master) wanted |= PCI_CMD_MASTER;
    else wanted &= (uint16_t)~PCI_CMD_MASTER;
    if (dev_command_write_checked(dev, wanted) == 0) return 0;

    /* No driver register or private DMA address has been touched yet. Restore
     * the firmware/previous-owner value; failure stays visible to the caller. */
    uint16_t after = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, old);
    uint16_t restored = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    kprintf("[device] PCI %02x:%02x.%u Command enable rejected "
            "(want=%x got=%x restore=%x%s)\n",
            dev->bus, dev->slot, dev->func, wanted, after, restored,
            restored == old ? "" : " FAILED");
    return -1;
}

int dev_disable_checked(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI) return -1;
    uint16_t old = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    if (old == UINT16_MAX) {
        kprintf("[device] PCI %02x:%02x.%u Command unavailable during disable\n",
                dev->bus, dev->slot, dev->func);
        return -1;
    }
    uint16_t wanted = (uint16_t)(old & ~(PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER));
    if (dev_command_write_checked(dev, wanted) == 0) return 0;
    uint16_t after = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    /* Never restore enable bits after a failed containment request. */
    kprintf("[device] PCI %02x:%02x.%u Command disable rejected "
            "(want=%x got=%x)\n",
            dev->bus, dev->slot, dev->func, wanted, after);
    return -1;
}

void dev_enable(struct device *dev, int bus_master)
{
    (void)dev_enable_checked(dev, bus_master);
}

void dev_disable(struct device *dev)
{
    (void)dev_disable_checked(dev);
}

uint64_t dev_bar_map(struct device *dev, int idx)
{
    if (!dev || idx < 0 || idx >= DEV_NRES) return 0;
    struct dev_resource *r = &dev->res[idx];
    if (!(r->flags & DEV_RES_MEM) || !r->start || !r->size) return 0;
    /* Identity map, uncached: MMIO must not be cached and the kernel's low
     * memory is identity-mapped anyway, so virt == phys keeps every driver's
     * pointer arithmetic honest. */
    vmm_map_range(r->start, r->start, r->size, VMM_WRITABLE | VMM_NOCACHE);
    return r->start;
}

/* ------------------------------------------------------------------ dump -- */
static const char *irq_mode_name(uint8_t m)
{
    switch (m) {
    case DEV_IRQ_MSIX: return "msix";
    case DEV_IRQ_MSI:  return "msi";
    case DEV_IRQ_INTX: return "intx";
    default:           return "-";
    }
}

void dev_dump(void)
{
    int bound = 0, busy = 0, count = dev_count();
    for (int i = 0; i < count; i++) {
        struct device *live = &g_devs[i];
        if (!dev_binding_try(live)) {
            busy++; kprintf("[dev] %s binding/teardown in progress\n", live->name); continue;
        }
        struct device snapshot = *live;
        dev_binding_put(live);
        struct device *d = &snapshot;
        if (d->drv) bound++;
        /* One line per function, machine-greppable: the boot tests assert on
         * `class=XX.YY` and `driver=NAME`, so this format is load-bearing. */
        kprintf("[dev] %s %04x:%04x class=%02x.%02x.%02x %-12s driver=%s",
                d->name, d->vendor, d->device,
                d->class_code, d->subclass, d->prog_if,
                pci_class_name(d->class_code, d->subclass),
                d->drv ? d->drv->name : "-");
        if (d->cap_msix || d->cap_msi || d->cap_pcie || d->cap_vendor)
            kprintf(" caps=%s%s%s%s",
                    d->cap_msix ? "msix," : "", d->cap_msi ? "msi," : "",
                    d->cap_pcie ? "pcie," : "", d->cap_vendor ? "vndr," : "");
        if (d->irq_mode != DEV_IRQ_NONE)
            kprintf(" irq=%s/%d", irq_mode_name(d->irq_mode), (int)d->irq_vec);
        for (int b = 0; b < DEV_NRES; b++) {
            if (!d->res[b].flags) continue;
            kprintf(" bar%d=%s%p/%p", b,
                    (d->res[b].flags & DEV_RES_IO) ? "io:" : "",
                    (void *)d->res[b].start, (void *)d->res[b].size);
        }
        kprintf("\n");
    }
    kprintf("[dev] %d device(s), %d bound, %d unclaimed\n",
            count, bound, count - bound - busy);
    if (busy) kprintf("[dev] %d binding snapshot(s) deferred\n", busy);
}
