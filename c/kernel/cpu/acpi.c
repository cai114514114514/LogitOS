/* Minimal ACPI: find the RSDP, walk the (X)SDT, and read out the tables the
 * kernel needs -- the MADT (Local APIC IDs, LAPIC/IOAPIC bases, interrupt source
 * overrides) and the MCFG (the PCIe ECAM window, used by c/kernel/pci).
 * Tables live in low RAM (< 512 MiB on our QEMU), which boot.asm identity-maps,
 * so physical == virtual here.
 *
 * The (X)SDT walk is split out of acpi_init() into acpi_find_table() because
 * the PCI bus driver needs the MCFG *before* smp_init() runs -- enumeration has
 * to happen early, SMP does not. acpi_tables_init() is idempotent, so whoever
 * gets there first pays for the RSDP search. */
#include <stdint.h>
#include <stddef.h>
#include "acpi.h"
#include "serial.h"

int  memcmp(const void *, const void *, size_t);

struct rsdp {
    char     sig[8];          /* "RSD PTR " */
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;        /* 0 = ACPI 1.0, >=2 = 2.0+ (has xsdt) */
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision, checksum;
    char     oemid[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
} __attribute__((packed));

static uint32_t g_lapic_base = 0xFEE00000;   /* default; MADT may override */
static uint8_t  g_apic_ids[ACPI_MAX_CPUS];
static int      g_ncpu;

static uint32_t g_ioapic_addr;               /* IOAPIC MMIO base (0 if none) */
static uint32_t g_ioapic_gsibase;
static uint32_t g_irq_gsi[16];               /* ISA IRQ -> GSI (identity unless overridden) */
static uint16_t g_irq_flags[16];             /* override polarity/trigger flags */

uint32_t acpi_lapic_base(void) { return g_lapic_base; }
int      acpi_cpu_count(void)  { return g_ncpu; }
uint8_t  acpi_cpu_apic_id(int i){ return (i >= 0 && i < g_ncpu) ? g_apic_ids[i] : 0; }
uint32_t acpi_ioapic_addr(void){ return g_ioapic_addr; }
uint32_t acpi_ioapic_gsibase(void){ return g_ioapic_gsibase; }
uint32_t acpi_gsi_for_irq(int irq){ return (irq >= 0 && irq < 16) ? g_irq_gsi[irq] : (uint32_t)irq; }
uint16_t acpi_gsi_flags(int irq){ return (irq >= 0 && irq < 16) ? g_irq_flags[irq] : 0; }

static int sum_ok(const void *p, int len)
{
    const uint8_t *b = p; uint8_t s = 0;
    for (int i = 0; i < len; i++) s += b[i];
    return s == 0;
}

static struct rsdp *find_rsdp(void)
{
    /* RSDP is on a 16-byte boundary in the EBDA or the BIOS area 0xE0000-0xFFFFF. */
    for (uint64_t a = 0x000E0000; a < 0x00100000; a += 16) {
        struct rsdp *r = (struct rsdp *)a;
        if (memcmp(r->sig, "RSD PTR ", 8) == 0 && sum_ok(r, 20))
            return r;
    }
    return NULL;
}

static void parse_madt(const struct sdt_header *madt)
{
    if (madt->length < 44) return;                       /* fixed MADT header must be present */
    const uint8_t *p = (const uint8_t *)madt;
    g_lapic_base = *(const uint32_t *)(p + 36);          /* MADT local APIC address */
    for (int i = 0; i < 16; i++) { g_irq_gsi[i] = (uint32_t)i; g_irq_flags[i] = 0; }
    const uint8_t *e = p + madt->length;
    p += 44;                                             /* skip MADT fixed header */
    while (p + 2 <= e) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > e) break;
        if (type == 0 && len >= 8) {                     /* Processor Local APIC */
            uint8_t apic_id = p[3];
            uint32_t flags = *(const uint32_t *)(p + 4);
            if ((flags & 1) && g_ncpu < ACPI_MAX_CPUS)   /* enabled */
                g_apic_ids[g_ncpu++] = apic_id;
        } else if (type == 1 && len >= 12) {             /* I/O APIC */
            if (!g_ioapic_addr) {
                g_ioapic_addr    = *(const uint32_t *)(p + 4);
                g_ioapic_gsibase = *(const uint32_t *)(p + 8);
            }
        } else if (type == 2 && len >= 10) {             /* Interrupt Source Override */
            uint8_t src = p[3];
            if (src < 16) {
                g_irq_gsi[src]   = *(const uint32_t *)(p + 4);
                g_irq_flags[src] = *(const uint16_t *)(p + 8);
            }
        } else if (type == 5 && len >= 12) {             /* LAPIC address override (64-bit) */
            uint64_t la = *(const uint64_t *)(p + 4);
            if ((la >> 32) == 0)                         /* only a 32-bit base is usable here */
                g_lapic_base = (uint32_t)la;
        }
        p += len;
    }
}

/* ------------------------------------------------------ (X)SDT table walk -- */
static const struct sdt_header *g_xsdt;   /* 64-bit entry table (ACPI 2.0+) */
static const struct sdt_header *g_rsdt;   /* 32-bit entry table (ACPI 1.0)  */
static int g_tables_done;

int acpi_tables_init(void)
{
    if (g_tables_done) return (g_xsdt || g_rsdt) ? 0 : -1;
    g_tables_done = 1;

    struct rsdp *r = find_rsdp();
    if (!r) { serial_puts("[acpi] no RSDP\n"); return -1; }

    if (r->revision >= 2 && r->xsdt_addr) {
        const struct sdt_header *x = (const struct sdt_header *)r->xsdt_addr;
        if (x->length < sizeof *x) { serial_puts("[acpi] bad XSDT length\n"); return -1; }
        g_xsdt = x;
    } else if (r->rsdt_addr) {
        const struct sdt_header *t = (const struct sdt_header *)(uint64_t)r->rsdt_addr;
        if (t->length < sizeof *t) { serial_puts("[acpi] bad RSDT length\n"); return -1; }
        g_rsdt = t;
    } else {
        return -1;
    }
    return 0;
}

const void *acpi_find_table(const char *sig)
{
    if (acpi_tables_init() != 0) return NULL;
    if (g_xsdt) {
        int n = (int)((g_xsdt->length - sizeof *g_xsdt) / 8);
        const uint8_t *e = (const uint8_t *)g_xsdt + sizeof *g_xsdt;
        for (int i = 0; i < n; i++) {
            uint64_t a;                     /* the XSDT entry array is only
                                             * 4-byte aligned in practice */
            const uint8_t *p = e + i * 8;
            a = 0; for (int b = 0; b < 8; b++) a |= (uint64_t)p[b] << (b * 8);
            const struct sdt_header *h = (const struct sdt_header *)a;
            if (h && memcmp(h->sig, sig, 4) == 0) return h;
        }
    } else if (g_rsdt) {
        int n = (int)((g_rsdt->length - sizeof *g_rsdt) / 4);
        const uint32_t *ent = (const uint32_t *)((const uint8_t *)g_rsdt + sizeof *g_rsdt);
        for (int i = 0; i < n; i++) {
            const struct sdt_header *h = (const struct sdt_header *)(uint64_t)ent[i];
            if (h && memcmp(h->sig, sig, 4) == 0) return h;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ MCFG --
 * "PCI Express Memory Mapped Configuration" table: a 44-byte header (36-byte
 * SDT header + 8 reserved) followed by 16-byte allocation entries
 *   u64 base, u16 segment, u8 bus_start, u8 bus_end, u32 reserved
 * Each entry is one ECAM window. We report them by index; the PCI bus driver
 * takes segment 0 and maps it. */
int acpi_mcfg_entry(int idx, uint64_t *base, uint16_t *seg, uint8_t *bus_lo, uint8_t *bus_hi)
{
    const struct sdt_header *h = acpi_find_table("MCFG");
    if (!h || h->length < 44 || idx < 0) return -1;
    int n = (int)((h->length - 44) / 16);
    if (idx >= n) return -1;
    const uint8_t *p = (const uint8_t *)h + 44 + idx * 16;
    uint64_t b = 0; for (int i = 0; i < 8; i++) b |= (uint64_t)p[i] << (i * 8);
    if (base)   *base   = b;
    if (seg)    *seg    = (uint16_t)(p[8] | (p[9] << 8));
    if (bus_lo) *bus_lo = p[10];
    if (bus_hi) *bus_hi = p[11];
    return 0;
}

int acpi_init(void)
{
    if (acpi_tables_init() != 0) return -1;
    const struct sdt_header *madt = acpi_find_table("APIC");
    if (!madt) { serial_puts("[acpi] no MADT\n"); return -1; }

    parse_madt(madt);
    return g_ncpu;
}
