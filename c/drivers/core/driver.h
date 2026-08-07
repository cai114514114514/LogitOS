#ifndef LOGIT_DRIVER_H
#define LOGIT_DRIVER_H
/* ===========================================================================
 * LogitOS device model -- the bus/device/driver contract.
 *
 * WHY THIS EXISTS
 *   Before this file the only way to reach hardware was
 *       int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);
 *   i.e. you had to already know the exact vendor:device ID, and you got back
 *   bus 0 / function 0 / BAR0 only. Anything else -- a device behind a bridge,
 *   a driver that wants "any AHCI controller", a second NIC, an interrupt that
 *   is not a hand-wired GSI -- was not merely hard, it was unexpressible.
 *
 * THE CONTRACT (three pieces)
 *   1. ENUMERATION happens once, up front, and is bus-driver work, not driver
 *      work. c/kernel/pci walks every bus (recursing through PCI-to-PCI
 *      bridges), decodes class/subclass/prog-if, sizes every BAR, and finds the
 *      capability offsets. Each function found becomes one `struct device` in a
 *      global registry.
 *   2. A DRIVER is a `struct driver` with a MATCH TABLE. It never scans. It
 *      declares what it can drive -- by vendor:device, by class/subclass, or by
 *      both -- and `dev_probe_all()` calls its ->probe() once per matching
 *      device. A driver is therefore a self-contained file: add the file, add
 *      nothing else.
 *   3. INTERRUPTS come from the model, not from the driver. `dev_irq_request()`
 *      picks MSI-X, then MSI, then legacy INTx through the I/O APIC, allocates
 *      a CPU vector, installs the IDT gate and unmasks. The driver passes a
 *      function pointer and never touches a GSI.
 *
 * HOW TO WRITE A DRIVER (the whole of it)
 *
 *      #include "driver.h"
 *
 *      static int ahci_probe(struct device *dev) {
 *          uint64_t abar = dev_bar_map(dev, 5);       // maps + returns base
 *          if (!abar) return -1;                      // != 0 -> not bound
 *          dev_irq_request(dev, ahci_isr, dev, "ahci");
 *          dev_set_drvdata(dev, ...);
 *          return 0;                                  // 0 -> bound
 *      }
 *
 *      static const struct dev_match ahci_ids[] = {
 *          DEV_MATCH_CLASS(PCI_CLASS_STORAGE, 0x06),  // any AHCI controller
 *          DEV_MATCH_END
 *      };
 *
 *      static struct driver ahci_driver = {
 *          .name = "ahci", .bus_type = DEV_BUS_PCI,
 *          .match = ahci_ids, .probe = ahci_probe,
 *      };
 *      DRIVER_DECLARE(ahci_driver);   // linker section; no central list to edit
 *
 *   probe() returns 0 to bind, non-zero to decline (the device stays free for
 *   another driver). Probe order is: drivers are tried in registration order,
 *   and the first that returns 0 owns the device. A device is bound at most
 *   once.
 *
 * WHAT THIS DELIBERATELY IS NOT
 *   No hotplug, no removal at runtime (->remove exists for symmetry and is
 *   called only by dev_unbind()), no power management, no DMA API, no
 *   per-device locking. Enumeration is a single pass at boot. Those are all
 *   additions this shape allows; none of them are here.
 *
 * OWNERSHIP: c/drivers/core (the model), c/kernel/pci (the PCI bus driver).
 * =========================================================================== */

#include <stdint.h>

/* ---------------------------------------------------------------- buses -- */
#define DEV_BUS_PCI      1
#define DEV_BUS_PLATFORM 2      /* reserved: ISA/legacy devices, not enumerated */

/* ------------------------------------------------------------ resources -- */
#define DEV_RES_MEM      0x01   /* memory-mapped BAR */
#define DEV_RES_IO       0x02   /* I/O-port BAR */
#define DEV_RES_64       0x04   /* 64-bit memory BAR (consumes the next index) */
#define DEV_RES_PREFETCH 0x08

struct dev_resource {
    uint64_t start;             /* physical base; 0 == this BAR is unimplemented */
    uint64_t size;              /* decoded size in bytes (BAR write-ones probe) */
    uint32_t flags;             /* DEV_RES_* (0 == unused) */
};

/* --------------------------------------------------------------- device -- */
#define DEV_NAME_LEN 16         /* "0000:00:1f.2" + NUL */
#define DEV_NRES     6          /* PCI type-0 header has 6 BARs */

/* How dev_irq_request() actually wired the interrupt. */
#define DEV_IRQ_NONE 0
#define DEV_IRQ_INTX 1
#define DEV_IRQ_MSI  2
#define DEV_IRQ_MSIX 3

struct driver;
struct dev_match;

struct device {
    char     name[DEV_NAME_LEN];    /* "0000:00:03.0" -- seg:bus:slot.func */
    uint8_t  bus_type;              /* DEV_BUS_* */

    /* PCI address + identity (valid when bus_type == DEV_BUS_PCI) */
    uint16_t seg;
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint16_t subsys_vendor, subsys_device;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;           /* 0 = endpoint, 1 = PCI-to-PCI bridge */

    struct dev_resource res[DEV_NRES];

    /* Capability offsets in config space, 0 == the device has no such cap. */
    uint8_t  cap_msi;               /* 0x05 */
    uint8_t  cap_msix;              /* 0x11 */
    uint8_t  cap_pcie;              /* 0x10 */
    uint8_t  cap_vendor;            /* 0x09 -- first vendor-specific (virtio) */

    /* Interrupt wiring (filled by dev_irq_request) */
    uint8_t  irq_pin;               /* config 0x3D: 1..4 = INTA..INTD, 0 = none */
    uint8_t  irq_line;              /* config 0x3C: BIOS-programmed legacy IRQ */
    uint8_t  irq_mode;              /* DEV_IRQ_* */
    int16_t  irq_vec;               /* CPU vector, -1 when unwired */

    /* Binding */
    const struct driver    *drv;    /* NULL until a probe() returned 0 */
    const struct dev_match *match;  /* the table entry that matched (->data) */
    void                   *drvdata;
};

/* ---------------------------------------------------------- match tables -- */
/* Wildcards. Vendor 0xFFFF is what an absent PCI function reads back as, and
 * class 0xFF is "unassigned", so neither can collide with a real value. */
#define DEV_ANY   0xFFFFu
#define DEV_ANYC  0xFFu

struct dev_match {
    uint16_t vendor, device;                 /* DEV_ANY  = don't care */
    uint8_t  class_code, subclass, prog_if;  /* DEV_ANYC = don't care */
    uint32_t data;                           /* driver-private tag, e.g. a chip
                                              * variant; readable via
                                              * dev->match->data in probe() */
};

/* An all-zero entry terminates a table. A real entry always has at least one
 * wildcard (0xFFFF/0xFF) or a nonzero vendor, so this is unambiguous. */
#define DEV_MATCH_END               { 0, 0, 0, 0, 0, 0 }
#define DEV_MATCH_VD(v, d)          { (v), (d), DEV_ANYC, DEV_ANYC, DEV_ANYC, 0 }
#define DEV_MATCH_VD_DATA(v, d, t)  { (v), (d), DEV_ANYC, DEV_ANYC, DEV_ANYC, (t) }
#define DEV_MATCH_CLASS(c, s)       { DEV_ANY, DEV_ANY, (c), (s), DEV_ANYC, 0 }
#define DEV_MATCH_PROGIF(c, s, p)   { DEV_ANY, DEV_ANY, (c), (s), (p), 0 }
/* vendor + class together, e.g. "any Intel storage controller" */
#define DEV_MATCH_VCLASS(v, c, s)   { (v), DEV_ANY, (c), (s), DEV_ANYC, 0 }

/* --------------------------------------------------------------- driver -- */
struct driver {
    const char *name;                       /* short, appears in the boot log */
    uint8_t     bus_type;                   /* DEV_BUS_PCI */
    const struct dev_match *match;          /* DEV_MATCH_END-terminated */
    int  (*probe)(struct device *dev);      /* 0 = bound, !=0 = decline */
    void (*remove)(struct device *dev);     /* optional */
    struct driver *next;                    /* internal: registry chain */
};

/* Explicit registration (always available; used by host tests). */
void driver_register(struct driver *drv);

/* Declarative registration: emits a pointer into the `logit_drivers` linker
 * section, which linker.ld brackets with __start_/__stop_logit_drivers. This is
 * what makes a driver a self-contained file -- there is no central list. */
#define DRIVER_DECLARE(sym)                                                   \
    static struct driver *const __drvref_##sym                                \
        __attribute__((used, section("logit_drivers"))) = &(sym)

/* ------------------------------------------------------------ the model -- */

/* Add a device to the registry (called by a bus driver during enumeration).
 * Returns the stored device, or NULL if the registry is full. */
struct device *dev_add(const struct device *proto);

int             dev_count(void);
struct device  *dev_at(int i);

/* Look a device up by class/subclass or vendor/device; `from` is an iteration
 * cursor (NULL to start). Returns NULL when no more match. */
struct device *dev_find_class(uint8_t class_code, uint8_t subclass, struct device *from);
struct device *dev_find_id(uint16_t vendor, uint16_t device, struct device *from);

/* Does `m` (a single table entry) describe `dev`? Exposed because it is the
 * one piece of resolution logic worth unit-testing on its own. */
int dev_match_one(const struct dev_match *m, const struct device *dev);
/* First matching entry of a DEV_MATCH_END-terminated table, or NULL. */
const struct dev_match *dev_match_table(const struct dev_match *tbl, const struct device *dev);

/* Register every DRIVER_DECLARE'd driver, then run one probe/bind pass over
 * every unbound device. Safe to call again after registering more drivers.
 * Returns the number of devices newly bound. */
int  dev_probe_all(void);
void dev_unbind(struct device *dev);

static inline void  dev_set_drvdata(struct device *d, void *p) { d->drvdata = p; }
static inline void *dev_get_drvdata(const struct device *d)    { return d->drvdata; }

/* Print the registry: one line per device with class name and bound driver.
 * This is the first debugging tool on unfamiliar hardware, so it is not
 * optional and not behind a debug flag. */
void dev_dump(void);

/* ----------------------------------------------------------- resources -- */
/* Map res[idx] into the kernel address space (identity, uncached) and return
 * its base address, or 0 if that BAR is absent / is an I/O BAR. Idempotent. */
uint64_t dev_bar_map(struct device *dev, int idx);

/* Enable/disable the device's response to memory + I/O cycles and its ability
 * to act as a DMA master (PCI command register). Every probe() that touches a
 * BAR or does DMA must call dev_enable(). */
void dev_enable(struct device *dev, int bus_master);
void dev_disable(struct device *dev);

/* ---------------------------------------------------------- interrupts -- */
typedef void (*irq_handler_t)(void *arg);

/* Wire an interrupt for `dev`: MSI-X, else MSI, else legacy INTx via the I/O
 * APIC. Allocates a CPU vector, installs the IDT gate, unmasks, and records
 * dev->irq_mode / dev->irq_vec. Returns the vector, or -1 if nothing could be
 * wired. `name` shows up in the boot log and in irq statistics.
 *
 * The handler runs in interrupt context on the BSP with the BKL held; EOI is
 * sent for you. It must not block and must not use floating point. */
int  dev_irq_request(struct device *dev, irq_handler_t fn, void *arg, const char *name);
void dev_irq_release(struct device *dev);

/* Delivery count for dev's vector (0 if unwired). Tests assert on this. */
uint64_t dev_irq_count(const struct device *dev);

/* Cap what dev_irq_request() is allowed to choose, globally. DEV_IRQ_MSIX (the
 * default) means "best available"; DEV_IRQ_MSI forbids MSI-X; DEV_IRQ_INTX
 * forces the legacy line. This exists so the fallback path can be exercised on
 * purpose -- a fallback that has never run is not a fallback. Returns the
 * previous setting. */
int dev_irq_prefer(int max_mode);

#endif /* LOGIT_DRIVER_H */
