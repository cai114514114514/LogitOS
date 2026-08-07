/* PCI bus driver.
 *
 * Two config-space mechanisms:
 *   - ECAM (PCIe "enhanced configuration access mechanism"): a memory window
 *     described by the ACPI MCFG table, addressed as
 *     base + (bus << 20) + (slot << 15) + (func << 12) + off. This is the only
 *     way to reach the extended config space (offsets 0x100..0xFFF, where the
 *     PCIe extended capabilities live) and the only way to reach a non-zero PCI
 *     segment at all.
 *   - The legacy 0xCF8/0xCFC port pair, which reaches bus 0..255 of segment 0
 *     but only offsets 0x00..0xFF.
 * ECAM is preferred and the ports are the fallback, so a machine with no MCFG
 * (QEMU's default i440fx, for one) still enumerates fully.
 *
 * Enumeration recurses through PCI-to-PCI bridges rather than brute-forcing all
 * 256 buses: a bridge's secondary-bus register is the authoritative statement of
 * which bus hangs off it, and a bus_seen bitmap makes a malformed topology
 * terminate instead of looping.
 *
 * Nothing here knows about any particular device. Each function found is handed
 * to the device model (dev_add), which is what decides who drives it. */
#include <stdint.h>
#include <stddef.h>
#include "pci.h"
#include "driver.h"
#include "io.h"
#include "vmm.h"
#include "kprintf.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* ------------------------------------------------------------------ ECAM -- */
static volatile uint8_t *g_ecam;        /* mapped window base, NULL = not in use */
static uint64_t g_ecam_phys;
static uint16_t g_ecam_seg;
static uint8_t  g_ecam_bus_lo, g_ecam_bus_hi;

int      pci_ecam_active(void) { return g_ecam != NULL; }
uint64_t pci_ecam_base(void)   { return g_ecam_phys; }

static uint8_t g_ecam_mapped[32];        /* bit per bus: its 1 MiB is mapped */

int pci_ecam_set(uint64_t base, uint16_t seg, uint8_t bus_start, uint8_t bus_end)
{
    if (!base || bus_end < bus_start) return 0;
    g_ecam        = (volatile uint8_t *)(uintptr_t)base;
    g_ecam_phys   = base;
    g_ecam_seg    = seg;
    g_ecam_bus_lo = bus_start;
    g_ecam_bus_hi = bus_end;
    for (unsigned i = 0; i < sizeof g_ecam_mapped; i++) g_ecam_mapped[i] = 0;
    return 1;
}

/* Byte address of (bus,slot,func,off) inside the ECAM window, or NULL if this
 * access cannot be served from it.
 *
 * Buses are mapped lazily, one 1 MiB window at a time. A full MCFG covers buses
 * 0..255 = 256 MiB, and eagerly mapping that costs ~512 KiB of page tables for
 * buses that will read back all-ones -- on a machine where a handful of buses
 * exist. The bitmap keeps the common path to one test. */
static volatile uint8_t *ecam_ptr(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    if (!g_ecam || bus < g_ecam_bus_lo || bus > g_ecam_bus_hi) return NULL;
    if (slot > 31 || func > 7 || off > 0xFFF) return NULL;
    if (!(g_ecam_mapped[bus >> 3] & (1u << (bus & 7)))) {
        uint64_t p = g_ecam_phys + ((uint64_t)(bus - g_ecam_bus_lo) << 20);
        vmm_map_range(p, p, 1u << 20, VMM_WRITABLE | VMM_NOCACHE);
        g_ecam_mapped[bus >> 3] |= (uint8_t)(1u << (bus & 7));
    }
    uint64_t o = ((uint64_t)(bus - g_ecam_bus_lo) << 20) | ((uint64_t)slot << 15) |
                 ((uint64_t)func << 12) | off;
    return g_ecam + o;
}

/* ------------------------------------------------------- config accessors -- */
static uint32_t port_addr(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    return (uint32_t)((1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                      ((uint32_t)func << 8) | (off & 0xFC));
}

uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    volatile uint8_t *p = ecam_ptr(bus, slot, func, (uint16_t)(off & ~3u));
    if (p) return *(volatile uint32_t *)p;
    if (off > 0xFF) return 0xFFFFFFFFu;      /* extended space needs ECAM */
    outl(PCI_CONFIG_ADDR, port_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint32_t val)
{
    volatile uint8_t *p = ecam_ptr(bus, slot, func, (uint16_t)(off & ~3u));
    if (p) { *(volatile uint32_t *)p = val; return; }
    if (off > 0xFF) return;
    outl(PCI_CONFIG_ADDR, port_addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    return (uint16_t)(pci_cfg_read(bus, slot, func, (uint16_t)(off & ~1u)) >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    return (uint8_t)(pci_cfg_read(bus, slot, func, off) >> ((off & 3) * 8));
}

void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint16_t val)
{
    uint16_t d = (uint16_t)(off & ~3u);
    uint32_t v = pci_cfg_read(bus, slot, func, d);
    int sh = (off & 2) * 8;
    v = (v & ~(0xFFFFu << sh)) | ((uint32_t)val << sh);
    pci_cfg_write(bus, slot, func, d, v);
}

void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint8_t val)
{
    uint16_t d = (uint16_t)(off & ~3u);
    uint32_t v = pci_cfg_read(bus, slot, func, d);
    int sh = (off & 3) * 8;
    v = (v & ~(0xFFu << sh)) | ((uint32_t)val << sh);
    pci_cfg_write(bus, slot, func, d, v);
}

/* ------------------------------------------------------------ capabilities -- */
uint8_t pci_cap_next(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id, uint8_t prev)
{
    if (!(pci_cfg_read16(bus, slot, func, PCI_CFG_STATUS) & PCI_STATUS_CAPLIST))
        return 0;
    /* A device with a corrupt (or hostile) capability chain can point back at an
     * earlier node. Record every offset we have visited; a repeat ends the walk.
     * The 48-step bound alone is not enough -- it would still read the same node
     * 48 times and could return a bogus hit, and a two-node cycle would make
     * pci_cap_next() return the same offset forever, hanging its caller's loop. */
    uint8_t seen[32] = { 0 };
    uint8_t off = pci_cfg_read8(bus, slot, func, PCI_CFG_CAP_PTR) & 0xFC;
    int past = (prev == 0);
    for (int i = 0; i < 48 && off >= 0x40; i++) {
        if (seen[off >> 3] & (1u << (off & 7))) break;    /* loop */
        seen[off >> 3] |= (uint8_t)(1u << (off & 7));
        uint32_t d = pci_cfg_read(bus, slot, func, off);
        if ((d & 0xFF) == 0xFF) break;                    /* absent device */
        if (past && (d & 0xFF) == cap_id) return off;
        if (off == prev) past = 1;
        off = (uint8_t)((d >> 8) & 0xFC);
    }
    return 0;
}

uint8_t pci_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id)
{
    return pci_cap_next(bus, slot, func, cap_id, 0);
}

uint16_t pci_ext_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint16_t cap_id)
{
    if (!g_ecam) return 0;
    uint8_t seen[512] = { 0 };                            /* 0x000..0xFFF / 8 */
    uint16_t off = 0x100;
    for (int i = 0; i < 64 && off >= 0x100 && off < 0x1000; i++) {
        if (seen[off >> 3] & (1u << (off & 7))) break;
        seen[off >> 3] |= (uint8_t)(1u << (off & 7));
        uint32_t d = pci_cfg_read(bus, slot, func, off);
        if (d == 0 || d == 0xFFFFFFFFu) break;
        if ((d & 0xFFFF) == cap_id) return off;
        off = (uint16_t)((d >> 20) & 0xFFC);
    }
    return 0;
}

/* ------------------------------------------------------------------ BARs -- */
int pci_bar_probe(uint8_t bus, uint8_t slot, uint8_t func, int idx, struct dev_resource *out)
{
    out->start = 0; out->size = 0; out->flags = 0;
    if (idx < 0 || idx >= DEV_NRES) return 1;

    uint16_t o = (uint16_t)(PCI_CFG_BAR0 + idx * 4);
    /* Sizing writes all-ones into the BAR, which momentarily moves the device's
     * decode window on top of whatever else is there. Turn decode off first --
     * skipping this is how a BAR probe corrupts an unrelated device's MMIO. */
    uint16_t cmd = pci_cfg_read16(bus, slot, func, PCI_CFG_COMMAND);
    pci_cfg_write16(bus, slot, func, PCI_CFG_COMMAND,
                    (uint16_t)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEM)));

    uint32_t lo = pci_cfg_read(bus, slot, func, o);
    pci_cfg_write(bus, slot, func, o, 0xFFFFFFFFu);
    uint32_t lo_sz = pci_cfg_read(bus, slot, func, o);
    pci_cfg_write(bus, slot, func, o, lo);

    int consumed = 1;
    if (lo & 1) {                                   /* I/O BAR */
        uint32_t mask = lo_sz & ~0x3u & 0xFFFFu;
        if (mask) {
            out->start = lo & ~0x3u;
            out->size  = (uint64_t)((~mask + 1) & 0xFFFFu);
            out->flags = DEV_RES_IO;
        }
    } else {
        int type = (int)((lo >> 1) & 3);            /* 0 = 32-bit, 2 = 64-bit */
        int wide = (type == 2 && idx + 1 < DEV_NRES);
        uint64_t base = lo & ~0xFu;
        /* The size mask is the read-back with the low flag bits cleared. For a
         * 32-bit BAR only the low dword is implemented, so sign-extending it
         * with ones gives the same ~mask+1 arithmetic as the 64-bit case. */
        uint64_t mask = (uint64_t)(lo_sz & ~0xFu) | 0xFFFFFFFF00000000ull;
        /* "Unimplemented" is the read-back being zero across the BAR's own
         * width -- NOT the original value being zero (an implemented BAR that
         * firmware left unassigned also reads 0). */
        int impl = (lo_sz & ~0xFu) != 0;
        if (wide) {
            uint16_t o2 = (uint16_t)(o + 4);
            uint32_t hi = pci_cfg_read(bus, slot, func, o2);
            pci_cfg_write(bus, slot, func, o2, 0xFFFFFFFFu);
            uint32_t hi_sz = pci_cfg_read(bus, slot, func, o2);
            pci_cfg_write(bus, slot, func, o2, hi);
            base |= (uint64_t)hi << 32;
            mask = (uint64_t)(lo_sz & ~0xFu) | ((uint64_t)hi_sz << 32);
            impl = ((lo_sz & ~0xFu) | hi_sz) != 0;
            consumed = 2;
        }
        if (impl && mask != ~0ull) {
            out->start = base;
            out->size  = ~mask + 1;
            out->flags = DEV_RES_MEM
                       | (wide ? DEV_RES_64 : 0)
                       | ((lo & 0x8) ? DEV_RES_PREFETCH : 0);
        }
    }
    pci_cfg_write16(bus, slot, func, PCI_CFG_COMMAND, cmd);
    return consumed;
}

/* ---------------------------------------------------------------- classes -- */
const char *pci_class_name(uint8_t cls, uint8_t sub)
{
    switch (cls) {
    case 0x00: return "unclassified";
    case 0x01:
        switch (sub) {
        case 0x00: return "scsi";
        case 0x01: return "ide";
        case 0x05: return "ata";
        case 0x06: return "sata/ahci";
        case 0x07: return "sas";
        case 0x08: return "nvme";
        default:   return "storage";
        }
    case 0x02: return sub == 0x00 ? "ethernet" : "network";
    case 0x03: return sub == 0x00 ? "vga" : "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06:
        switch (sub) {
        case 0x00: return "host-bridge";
        case 0x01: return "isa-bridge";
        case 0x04: return "pci-bridge";
        case 0x09: return "pci-bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication";
    case 0x08: return "system";
    case 0x09: return "input";
    case 0x0A: return "docking";
    case 0x0B: return "processor";
    case 0x0C:
        switch (sub) {
        case 0x03: return "usb";
        case 0x05: return "smbus";
        default:   return "serial-bus";
        }
    case 0x0D: return "wireless";
    case 0x10: return "crypto";
    case 0xFF: return "unassigned";
    default:   return "other";
    }
}

/* ------------------------------------------------------------ enumeration -- */
static uint8_t g_bus_seen[32];
static int     g_enumerated;

static void hex2(char *p, unsigned v) { const char *h = "0123456789abcdef"; p[0] = h[(v >> 4) & 0xF]; p[1] = h[v & 0xF]; }

static void dev_name(struct device *d)
{
    /* "0000:00:1f.2" -- the address form every PCI tool on earth prints. */
    char *p = d->name;
    hex2(p, d->seg >> 8); hex2(p + 2, d->seg & 0xFF); p[4] = ':';
    hex2(p + 5, d->bus); p[7] = ':';
    hex2(p + 8, d->slot); p[10] = '.';
    p[11] = (char)('0' + (d->func & 7));
    p[12] = 0;
}

static void scan_bus(uint8_t bus);

static void scan_func(uint8_t bus, uint8_t slot, uint8_t func, uint32_t id)
{
    struct device d;
    for (unsigned i = 0; i < sizeof d; i++) ((uint8_t *)&d)[i] = 0;

    d.bus_type = DEV_BUS_PCI;
    d.seg = g_ecam ? g_ecam_seg : 0;
    d.bus = bus; d.slot = slot; d.func = func;
    d.vendor = (uint16_t)(id & 0xFFFF);
    d.device = (uint16_t)(id >> 16);

    uint32_t cls = pci_cfg_read(bus, slot, func, PCI_CFG_CLASS);
    d.revision   = (uint8_t)(cls & 0xFF);
    d.prog_if    = (uint8_t)((cls >> 8) & 0xFF);
    d.subclass   = (uint8_t)((cls >> 16) & 0xFF);
    d.class_code = (uint8_t)((cls >> 24) & 0xFF);
    d.header_type = (uint8_t)(pci_cfg_read8(bus, slot, func, PCI_CFG_HEADER_TYPE) & 0x7F);

    /* Type 1 (bridge) headers have 2 BARs, not 6, and their 0x2C is a different
     * field, so only read the type-0 layout for endpoints. */
    int nbar = (d.header_type == 0) ? DEV_NRES : 2;
    for (int i = 0; i < nbar; )
        i += pci_bar_probe(bus, slot, func, i, &d.res[i]);

    if (d.header_type == 0) {
        uint32_t ss = pci_cfg_read(bus, slot, func, PCI_CFG_SUBSYS);
        d.subsys_vendor = (uint16_t)(ss & 0xFFFF);
        d.subsys_device = (uint16_t)(ss >> 16);
    }
    d.irq_line = pci_cfg_read8(bus, slot, func, PCI_CFG_IRQ_LINE);
    d.irq_pin  = pci_cfg_read8(bus, slot, func, PCI_CFG_IRQ_PIN);
    d.irq_mode = DEV_IRQ_NONE;
    d.irq_vec  = -1;

    d.cap_msi    = pci_cap_find(bus, slot, func, PCI_CAP_MSI);
    d.cap_msix   = pci_cap_find(bus, slot, func, PCI_CAP_MSIX);
    d.cap_pcie   = pci_cap_find(bus, slot, func, PCI_CAP_PCIE);
    d.cap_vendor = pci_cap_find(bus, slot, func, PCI_CAP_VENDOR);

    dev_name(&d);
    dev_add(&d);

    if (d.header_type == 1) {                       /* PCI-to-PCI bridge */
        uint8_t sec = pci_cfg_read8(bus, slot, func, PCI_CFG_SEC_BUS);
        if (sec && sec != bus)
            scan_bus(sec);
    }
}

static void scan_bus(uint8_t bus)
{
    if (g_bus_seen[bus >> 3] & (1u << (bus & 7))) return;   /* already walked */
    g_bus_seen[bus >> 3] |= (uint8_t)(1u << (bus & 7));

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_cfg_read(bus, slot, 0, PCI_CFG_VENDOR);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint8_t hdr = pci_cfg_read8(bus, slot, 0, PCI_CFG_HEADER_TYPE);
        int nfunc = (hdr & 0x80) ? 8 : 1;                   /* multi-function bit */
        for (int f = 0; f < nfunc; f++) {
            uint32_t fid = (f == 0) ? id : pci_cfg_read(bus, slot, (uint8_t)f, PCI_CFG_VENDOR);
            if ((fid & 0xFFFF) == 0xFFFF) continue;
            scan_func(bus, slot, (uint8_t)f, fid);
        }
    }
}

int pci_enumerate(void)
{
    if (g_enumerated) return dev_count();
    g_enumerated = 1;
    for (unsigned i = 0; i < sizeof g_bus_seen; i++) g_bus_seen[i] = 0;
    scan_bus(0);
    return dev_count();
}

/* ------------------------------------------------------- legacy pci_find -- */
/* Kept for e1000/nvme/virtio, which still ask for an exact vendor:device. Once
 * enumeration has run this answers from the registry, so those drivers now also
 * find their device behind a bridge or on a non-zero bus -- without changing.
 * Before enumeration it falls back to the original bus-0 slot scan. */
int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    uint8_t bus = 0, slot = 0, func = 0;
    int found = 0;

    struct device *d = dev_find_id(vendor, device, NULL);
    if (d) { bus = d->bus; slot = d->slot; func = d->func; found = 1; }

    if (!found) {
        for (uint8_t s = 0; s < 32 && !found; s++) {
            uint32_t id = pci_cfg_read(0, s, 0, PCI_CFG_VENDOR);
            if ((id & 0xFFFF) == vendor && (id >> 16) == device) { slot = s; found = 1; }
        }
    }
    if (!found) return -1;

    out->bus = bus; out->slot = slot; out->func = func;
    out->vendor = vendor; out->device = device;
    out->bar0 = pci_cfg_read(bus, slot, func, PCI_CFG_BAR0) & ~(uint32_t)0xF;
    out->irq_line = pci_cfg_read8(bus, slot, func, PCI_CFG_IRQ_LINE);

    uint16_t cmd = pci_cfg_read16(bus, slot, func, PCI_CFG_COMMAND);
    pci_cfg_write16(bus, slot, func, PCI_CFG_COMMAND,
                    (uint16_t)(cmd | PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER));
    return 0;
}
