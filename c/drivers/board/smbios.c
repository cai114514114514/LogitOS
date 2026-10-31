/* SMBIOS reader (DMTF DSP0134 3.7, sections 5.2 and 6.1).
 *
 * Firmware wrote every byte here, and a board as cheap as the X79-VG4 is
 * exactly where a structure length is wrong or a string set is unterminated.
 * So every read is bounded by the table length the entry point declared, and a
 * structure that would cross it ends the walk instead of being trusted. */
#include <stddef.h>
#include "smbios.h"
#include "board_io.h"
#include "logit_boot.h"

#define SMBIOS_TABLE_MAX 0x10000u   /* 2.x tables are u16-sized; cap 3.x the same */

static int sum_ok(const uint8_t *p, uint32_t n)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

static int match(const uint8_t *p, const char *anchor)
{
    for (; *anchor; anchor++, p++) if (*p != (uint8_t)*anchor) return 0;
    return 1;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static uint64_t g_entry_phys;
/* The UEFI loader's copy (LOGIT_BOOT_TAG_SMBIOS), saved while the boot block
 * is still mapped; when present it is the only thing smbios_read() looks at. */
static uint8_t  g_copy_entry[0x20], g_copy_table[LOGIT_BOOT_SMBIOS_TABLE_MAX];
static uint32_t g_copy_elen, g_copy_tlen;

/* Firmware-described non-RAM regions (reserved, ACPI, NVS) from the boot
 * memory map. A checksum-valid entry point is still firmware data: its table
 * address is only followed when the whole extent lies in one of these, or in
 * the legacy BIOS area below 1 MiB, never into a hole or MMIO aperture. */
#define SMBIOS_MAX_REGIONS 64
static struct { uint64_t lo, hi; } g_fw[SMBIOS_MAX_REGIONS];
static int g_nfw;

__attribute__((unused)) static int firmware_extent(uint64_t phys, uint64_t len)
{
    if (!len || phys + len < phys) return 0;
    if (phys >= 0xe0000 && phys + len <= 0x100000) return 1;
    /* E820 "reserved" also covers MMIO; the local APIC, I/O APIC, HPET and
     * flash window sit at 0xFEC00000-4 GiB on every PC, and enumerated PCI
     * BARs are the rest. A table there is not a table. */
    if (phys < 0x100000000ull && phys + len > 0xfec00000ull) return 0;
    if (bio_device_aperture(phys, len)) return 0;
    for (int i = 0; i < g_nfw; i++)
        if (phys >= g_fw[i].lo && phys + len <= g_fw[i].hi) return 1;
    return 0;
}

void smbios_set_entry_phys(uint64_t phys) { g_entry_phys = phys; }

void smbios_capture_boot_info(uint64_t info)
{
    /* the normalized view: u32 total_size, u32 reserved, then 8-aligned tags,
     * end tag type 0; extension tags keep their LOGIT_BOOT_TAG_* type */
    g_entry_phys = 0; g_nfw = 0; g_copy_elen = g_copy_tlen = 0;
    if (!info) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)info;
    uint32_t total = rd32(base);
    if (total < 16 || total > 0x100000) return;
    for (uint32_t off = 8; off + 8 <= total; ) {
        uint32_t type = rd32(base + off), size = rd32(base + off + 4);
        if (type == 0 || size < 8 || size > total - off) return;
        if (type == LOGIT_BOOT_TAG_SMBIOS && size >= 16) {
            uint32_t elen = rd32(base + off + 8), tlen = rd32(base + off + 12);
            if (elen <= sizeof g_copy_entry && tlen <= sizeof g_copy_table &&
                16ull + elen + tlen <= size) {
                for (uint32_t k = 0; k < elen; k++) g_copy_entry[k] = base[off + 16 + k];
                for (uint32_t k = 0; k < tlen; k++) g_copy_table[k] = base[off + 16 + elen + k];
                g_copy_elen = elen; g_copy_tlen = tlen;
            }
        }
        if (type == 6 && size >= 16) {        /* the normalized memory-map tag */
            uint32_t esz = rd32(base + off + 8);
            for (uint32_t e = off + 16; esz >= 24 && e + 24 <= off + size; e += esz) {
                uint64_t a = rd32(base + e) | (uint64_t)rd32(base + e + 4) << 32;
                uint64_t l = rd32(base + e + 8) | (uint64_t)rd32(base + e + 12) << 32;
                uint32_t kind = rd32(base + e + 16);
                if ((kind == LOGIT_BOOT_MEMORY_RESERVED || kind == LOGIT_BOOT_MEMORY_ACPI_RECLAIM ||
                     kind == LOGIT_BOOT_MEMORY_NVS) && l && a + l > a && g_nfw < SMBIOS_MAX_REGIONS) {
                    g_fw[g_nfw].lo = a; g_fw[g_nfw].hi = a + l; g_nfw++;
                }
            }
        }
        off += (size + 7) & ~7u;
    }
}


int smbios_parse_entry(const uint8_t *ep, uint32_t avail, uint64_t *table_phys,
                       uint32_t *table_len, uint8_t *major, uint8_t *minor)
{
    if (avail >= 0x18 && match(ep, "_SM3_")) {           /* 64-bit, section 5.2.2 */
        uint8_t len = ep[6];
        if (len < 0x18 || len > avail || !sum_ok(ep, len)) return -1;
        *major = ep[7]; *minor = ep[8];
        *table_len = rd32(ep + 12);                       /* a maximum, not exact */
        *table_phys = rd32(ep + 16) | (uint64_t)rd32(ep + 20) << 32;
    } else if (avail >= 0x1f && match(ep, "_SM_")) {      /* 32-bit, section 5.2.1 */
        uint8_t len = ep[5];
        if (len < 0x1f || len > avail || !sum_ok(ep, len)) return -1;
        if (!match(ep + 16, "_DMI_") || !sum_ok(ep + 16, 15)) return -1;
        *major = ep[6]; *minor = ep[7];
        *table_len = rd16(ep + 22);
        *table_phys = rd32(ep + 24);
    } else {
        return -1;
    }
    if (!*table_phys || !*table_len) return -1;
    if (*table_len > SMBIOS_TABLE_MAX) *table_len = SMBIOS_TABLE_MAX;
    return 0;
}

/* String `index` (1-based) of the structure whose formatted area is [s, s+flen)
 * and whose string set ends before `end`. Copies at most cap-1 bytes. */
static void copy_string(const uint8_t *s, uint32_t flen, const uint8_t *end,
                        uint8_t index, char *dst, uint32_t cap)
{
    dst[0] = 0;
    if (!index) return;
    const uint8_t *p = s + flen;
    for (uint8_t i = 1; p < end && *p; i++) {
        const uint8_t *q = p;
        while (q < end && *q) q++;
        if (q >= end) return;                             /* unterminated string */
        if (i == index) {
            uint32_t n = 0;
            for (; p < q && n + 1 < cap; p++) {
                uint8_t c = *p;
                dst[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
            /* trailing blanks are common padding in cheap firmware */
            while (n && dst[n - 1] == ' ') n--;
            dst[n] = 0;
            return;
        }
        p = q + 1;
    }
}

static uint32_t dimm_size_mb(const uint8_t *s, uint8_t flen)
{
    uint16_t size = rd16(s + 0x0c);
    if (size == 0 || size == 0xffff) return 0;
    if (size == 0x7fff)                                   /* section 7.18.5 */
        return flen >= 0x20 ? (rd32(s + 0x1c) & 0x7fffffffu) : 0;
    return (size & 0x8000) ? (size & 0x7fffu) / 1024 : size;
}

int smbios_parse_table(const uint8_t *t, uint32_t len, struct smbios_info *out)
{
    const uint8_t *p = t, *end = t + len;
    int seen = 0;
    out->dimm_count = 0;
    out->bios_vendor[0] = out->bios_version[0] = out->bios_date[0] = 0;
    out->sys_vendor[0] = out->sys_product[0] = 0;
    out->board_vendor[0] = out->board_product[0] = out->board_version[0] = 0;

    while (p + 4 <= end) {
        uint8_t type = p[0], flen = p[1];
        if (flen < 4 || flen > (uint32_t)(end - p)) break;
        /* the string set ends at the first double NUL after the formatted area */
        const uint8_t *q = p + flen;
#ifndef BOARD_NEGCTL_SMBIOS_UNBOUNDED
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
        if (q + 1 >= end) break;                          /* runs off the table */
#else
        while (!(q[0] == 0 && q[1] == 0)) q++;
#endif
        const uint8_t *next = q + 2;
        switch (type) {
        case 0:
            if (flen >= 0x09) {
                copy_string(p, flen, next, p[0x04], out->bios_vendor, sizeof out->bios_vendor);
                copy_string(p, flen, next, p[0x05], out->bios_version, sizeof out->bios_version);
                copy_string(p, flen, next, p[0x08], out->bios_date, sizeof out->bios_date);
            }
            break;
        case 1:
            if (flen >= 0x06) {
                copy_string(p, flen, next, p[0x04], out->sys_vendor, sizeof out->sys_vendor);
                copy_string(p, flen, next, p[0x05], out->sys_product, sizeof out->sys_product);
            }
            break;
        case 2:
            if (flen >= 0x07) {
                copy_string(p, flen, next, p[0x04], out->board_vendor, sizeof out->board_vendor);
                copy_string(p, flen, next, p[0x05], out->board_product, sizeof out->board_product);
                copy_string(p, flen, next, p[0x06], out->board_version, sizeof out->board_version);
            }
            break;
        case 17:
            if (flen >= 0x15 && out->dimm_count < SMBIOS_MAX_DIMMS) {
                uint32_t mb = dimm_size_mb(p, flen);
                if (!mb) break;                           /* empty slot */
                struct smbios_dimm *d = &out->dimm[out->dimm_count++];
                d->size_mb = mb;
                d->speed_mts = flen >= 0x17 ? rd16(p + 0x15) : 0;
                copy_string(p, flen, next, p[0x10], d->locator, sizeof d->locator);
                copy_string(p, flen, next, p[0x11], d->bank, sizeof d->bank);
                d->vendor[0] = d->part[0] = 0;
                if (flen >= 0x1b) {
                    copy_string(p, flen, next, p[0x17], d->vendor, sizeof d->vendor);
                    copy_string(p, flen, next, p[0x1a], d->part, sizeof d->part);
                }
            }
            break;
        }
        seen++;
        if (type == 127) break;                           /* end-of-table */
        p = next;
    }
    return seen ? 0 : -1;
}

int smbios_read(struct smbios_info *out)
{
    uint64_t table = 0; uint32_t tlen = 0;
    uint8_t major = 0, minor = 0;
    int found = -1;
    if (g_copy_elen && g_copy_tlen) {
        /* the loader's copy: version from the entry, structures from the
         * copied table; the address inside the entry is never followed */
        if (smbios_parse_entry(g_copy_entry, g_copy_elen, &table, &tlen, &major, &minor)) return -1;
        out->major = major; out->minor = minor;
        return smbios_parse_table(g_copy_table, g_copy_tlen, out);
    }
#ifndef BOARD_NEGCTL_SMBIOS_ANY_ADDRESS
    if (g_entry_phys && firmware_extent(g_entry_phys, 0x20)) {
#else
    if (g_entry_phys) {
#endif
        const uint8_t *ep = bio_firmware(g_entry_phys, 0x20);
        found = ep ? smbios_parse_entry(ep, 0x20, &table, &tlen, &major, &minor) : -1;
    }
    if (found) {
        /* section 5.2: the anchor is paragraph-aligned in 0xF0000-0xFFFFF;
         * prefer the 64-bit entry when firmware publishes both */
        const uint8_t *seg = bio_firmware(0xf0000, 0x10000);
        for (uint32_t off = 0; seg && off + 0x18 <= 0x10000 && found; off += 16)
            if (match(seg + off, "_SM3_"))
                found = smbios_parse_entry(seg + off, 0x10000 - off, &table, &tlen, &major, &minor);
        for (uint32_t off = 0; seg && off + 0x1f <= 0x10000 && found; off += 16)
            if (match(seg + off, "_SM_"))
                found = smbios_parse_entry(seg + off, 0x10000 - off, &table, &tlen, &major, &minor);
    }
    if (found) return -1;
#ifndef BOARD_NEGCTL_SMBIOS_ANY_ADDRESS
    if (!firmware_extent(table, tlen)) return -3;     /* hole, MMIO or RAM */
#endif
    const uint8_t *t = bio_firmware(table, tlen);
    if (!t) return -2;
    out->major = major; out->minor = minor;
    return smbios_parse_table(t, tlen, out);
}
