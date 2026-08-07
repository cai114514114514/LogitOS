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

#define DEV_MAX 64

static struct device  g_devs[DEV_MAX];
static int            g_ndev;
static struct driver *g_drivers;        /* singly linked, registration order */
static struct driver *g_drivers_tail;

int            dev_count(void)   { return g_ndev; }
struct device *dev_at(int i)     { return (i >= 0 && i < g_ndev) ? &g_devs[i] : NULL; }

struct device *dev_add(const struct device *proto)
{
    if (g_ndev >= DEV_MAX) return NULL;
    struct device *d = &g_devs[g_ndev++];
    const uint8_t *s = (const uint8_t *)proto;
    uint8_t *t = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof *d; i++) t[i] = s[i];
    d->drv = NULL; d->match = NULL; d->drvdata = NULL;
    return d;
}

/* ------------------------------------------------------------- iteration -- */
static int idx_after(struct device *from)
{
    if (!from) return 0;
    int i = (int)(from - g_devs);
    return (i < 0 || i >= g_ndev) ? g_ndev : i + 1;
}

struct device *dev_find_class(uint8_t class_code, uint8_t subclass, struct device *from)
{
    for (int i = idx_after(from); i < g_ndev; i++)
        if (g_devs[i].class_code == class_code &&
            (subclass == DEV_ANYC || g_devs[i].subclass == subclass))
            return &g_devs[i];
    return NULL;
}

struct device *dev_find_id(uint16_t vendor, uint16_t device, struct device *from)
{
    for (int i = idx_after(from); i < g_ndev; i++)
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
    if (!drv || drv->next) return;                  /* already registered */
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
    for (int i = 0; i < g_ndev; i++) {
        struct device *d = &g_devs[i];
        if (d->drv) continue;
        for (struct driver *drv = g_drivers; drv; drv = drv->next) {
            if (drv->bus_type && drv->bus_type != d->bus_type) continue;
            const struct dev_match *m = dev_match_table(drv->match, d);
            if (!m) continue;
            d->drv = drv; d->match = m;             /* visible to probe() */
            if (drv->probe && drv->probe(d) == 0) { bound++; break; }
            d->drv = NULL; d->match = NULL; d->drvdata = NULL;
        }
    }
    return bound;
}

void dev_unbind(struct device *dev)
{
    if (!dev || !dev->drv) return;
    if (dev->drv->remove) dev->drv->remove(dev);
    dev_irq_release(dev);
    dev->drv = NULL; dev->match = NULL; dev->drvdata = NULL;
}

/* ------------------------------------------------------------- resources -- */
void dev_enable(struct device *dev, int bus_master)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI) return;
    uint16_t cmd = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_MEM;
    if (bus_master) cmd |= PCI_CMD_MASTER;
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, cmd);
}

void dev_disable(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI) return;
    uint16_t cmd = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    cmd = (uint16_t)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER));
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, cmd);
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
    int bound = 0;
    for (int i = 0; i < g_ndev; i++) {
        struct device *d = &g_devs[i];
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
            g_ndev, bound, g_ndev - bound);
}
