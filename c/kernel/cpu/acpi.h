#ifndef LOGIT_ACPI_H
#define LOGIT_ACPI_H
#include <stdint.h>

#define ACPI_MAX_CPUS 32

/* Hand acpi.c the Multiboot2 information block the bootloader passed in ebx.
 * Must be called before the first acpi_tables_init() (kernel_main does it right
 * after pmm_init). Both loaders supply an ACPI tag -- GRUB on BIOS and
 * c/boot/efi on UEFI -- and under UEFI it is the ONLY way to find the RSDP,
 * because there is no BIOS area left to scan. */
void        acpi_set_mb2_info(uint64_t mb_info);

/* Find the RSDP and cache the (X)SDT. Idempotent; 0 on success, -1 if there is
 * no usable ACPI. Callable before acpi_init() -- the PCI bus driver needs the
 * MCFG table at enumeration time, which is long before SMP comes up. */
int         acpi_tables_init(void);

/* Look up a table by its 4-character signature ("APIC", "MCFG", ...). Returns a
 * pointer to its SDT header, or NULL. */
const void *acpi_find_table(const char *sig);

/* MCFG allocation entry `idx` (one PCIe ECAM window). 0 on success, -1 when
 * there is no MCFG or idx is past the end. Any out pointer may be NULL. */
int         acpi_mcfg_entry(int idx, uint64_t *base, uint16_t *seg,
                            uint8_t *bus_lo, uint8_t *bus_hi);

/* Parse ACPI tables (RSDP -> MADT). Returns the number of enabled CPUs, or -1. */
int      acpi_init(void);
int      acpi_cpu_count(void);
uint8_t  acpi_cpu_apic_id(int i);
uint32_t acpi_lapic_base(void);
uint32_t acpi_ioapic_addr(void);          /* IOAPIC MMIO base (0 if none) */
uint32_t acpi_ioapic_gsibase(void);
uint32_t acpi_gsi_for_irq(int isa_irq);   /* ISA IRQ -> GSI (handles overrides) */
uint16_t acpi_gsi_flags(int isa_irq);     /* MPS INTI flags for the override */

/* ------------------------------------------------------------------ FADT --
 * Power management registers, for c/kernel/core/power.c. Parsed lazily from
 * acpi_find_table("FACP") the first time either accessor below is called, and
 * cached -- same idiom as the (X)SDT walk: whoever asks first pays for it.
 *
 * We read only the LEGACY 32-bit fields (PM1a/PM1b_CNT_BLK port numbers,
 * RESET_REG/RESET_VALUE), never the 64-bit X_ Generic Address Structures a
 * post-ACPI-2.0 FADT also carries. Every x86 FADT still populates the legacy
 * fields for compatibility even when the X_ ones are present, this machine is
 * always x86, and there is no reason to prefer the wider field when the
 * narrower one is guaranteed to already say the same thing. */

/* A Generic Address Structure, ACPI-spec shaped. `space_id` 0 = system
 * memory, 1 = system I/O -- acpi_reset_reg() below refuses to report a
 * register in any OTHER address space (PCI config, embedded controller,
 * SMBus, ...) because this kernel has no way to reach one; a caller sees "no
 * RESET_REG" instead of a register it cannot honestly write. */
struct acpi_gas {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
};

/* PM1a_CNT_BLK / PM1b_CNT_BLK: the I/O ports SLP_TYP/SLP_EN are written to for
 * S5. Returns 0 and fills `pm1a` (and `pm1b`, 0 if the FADT has none) if the
 * FADT is present and long enough to carry PM1a_CNT_BLK and it is nonzero;
 * -1 otherwise -- a truncated or absent table is refused, not trusted, same
 * as a truncated MADT entry above. */
int acpi_pm1_cnt(uint32_t *pm1a, uint32_t *pm1b);

/* RESET_REG / RESET_VALUE (ACPI 2.0+ FADT only -- absent from a revision-1
 * table, which is shorter than where these fields live). Returns 0 and fills
 * both if the table is long enough AND the register's address space is one
 * this kernel can act on (see struct acpi_gas above); -1 otherwise. */
int acpi_reset_reg(struct acpi_gas *reg, uint8_t *value);

#endif
