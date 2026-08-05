/* Minimal ACPI: find the RSDP, walk the (X)SDT to the MADT, and read out the
 * Local APIC IDs (the CPUs) + the LAPIC MMIO base. Enough to bring up SMP.
 * Tables live in low RAM (< 512 MiB on our QEMU), which boot.asm identity-maps,
 * so physical == virtual here. */
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

int acpi_init(void)
{
    struct rsdp *r = find_rsdp();
    if (!r) { serial_puts("[acpi] no RSDP\n"); return -1; }

    const struct sdt_header *madt = NULL;
    if (r->revision >= 2 && r->xsdt_addr) {              /* ACPI 2.0+: 64-bit XSDT */
        const struct sdt_header *xsdt = (const struct sdt_header *)r->xsdt_addr;
        if (xsdt->length < sizeof *xsdt) { serial_puts("[acpi] bad XSDT length\n"); return -1; }
        int n = (xsdt->length - sizeof *xsdt) / 8;
        const uint64_t *ent = (const uint64_t *)((const uint8_t *)xsdt + sizeof *xsdt);
        for (int i = 0; i < n; i++) {
            const struct sdt_header *h = (const struct sdt_header *)ent[i];
            if (memcmp(h->sig, "APIC", 4) == 0) { madt = h; break; }
        }
    } else {                                             /* ACPI 1.0: 32-bit RSDT */
        const struct sdt_header *rsdt = (const struct sdt_header *)(uint64_t)r->rsdt_addr;
        if (rsdt->length < sizeof *rsdt) { serial_puts("[acpi] bad RSDT length\n"); return -1; }
        int n = (rsdt->length - sizeof *rsdt) / 4;
        const uint32_t *ent = (const uint32_t *)((const uint8_t *)rsdt + sizeof *rsdt);
        for (int i = 0; i < n; i++) {
            const struct sdt_header *h = (const struct sdt_header *)(uint64_t)ent[i];
            if (memcmp(h->sig, "APIC", 4) == 0) { madt = h; break; }
        }
    }
    if (!madt) { serial_puts("[acpi] no MADT\n"); return -1; }

    parse_madt(madt);
    return g_ncpu;
}
