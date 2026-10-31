/* SMBIOS: who made this board, which firmware it runs, and what sits in its
 * memory slots (DMTF DSP0134 3.7). Read-only; nothing here writes firmware. */
#ifndef SMBIOS_H
#define SMBIOS_H
#include <stdint.h>

#define SMBIOS_MAX_DIMMS 8

struct smbios_dimm {
    char     locator[24];          /* type 17 Device Locator, e.g. "DIMM1" */
    char     bank[24];             /* type 17 Bank Locator, e.g. "Node0_Bank0" */
    char     vendor[32];
    char     part[32];
    uint32_t size_mb;              /* 0 = empty slot */
    uint16_t speed_mts;            /* 0 = unknown */
};

struct smbios_info {
    uint8_t  major, minor;
    char     bios_vendor[48], bios_version[48], bios_date[16];
    char     sys_vendor[48], sys_product[48];
    char     board_vendor[48], board_product[48], board_version[32];
    int      dimm_count;           /* populated slots only */
    struct smbios_dimm dimm[SMBIOS_MAX_DIMMS];
};

/* Validate an entry point ("_SM3_" or "_SM_" with its "_DMI_" tail) found in
 * `avail` readable bytes. 0 on success, with the structure table's location. */
int smbios_parse_entry(const uint8_t *ep, uint32_t avail, uint64_t *table_phys,
                       uint32_t *table_len, uint8_t *major, uint8_t *minor);

/* Decode types 0, 1, 2 and 17 from a structure table of exactly `len` bytes.
 * Never reads past `len`, whatever the lengths inside claim. 0 on success. */
int smbios_parse_table(const uint8_t *table, uint32_t len, struct smbios_info *out);

/* The UEFI loader can hand over the entry point's physical address (the BIOS
 * area holds nothing under UEFI); without one, the 0xF0000 segment is scanned. */
void smbios_set_entry_phys(uint64_t phys);
/* Take the loader's SMBIOS copy (UEFI) and the firmware memory regions from the boot
 * information block (the normalized tag list kernel_main receives) while it
 * is still identity-mapped. smbios_read() follows addresses only into those
 * regions or the BIOS area: -3 means the table points anywhere else. */
void smbios_capture_boot_info(uint64_t info);
int  smbios_read(struct smbios_info *out);

#endif
