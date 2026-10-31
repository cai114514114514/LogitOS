/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPD over the Sandy/Ivy Bridge-EP integrated memory controller's SMBus.
 *
 * Register layout and sequence from FreeBSD sys/dev/imcsmb (imcsmb.c,
 * imcsmb_reg.h, BSD-2-Clause, Panasas), which cites volume 2 of the Xeon
 * E5-1600/2600 (v1, v2) datasheets: iMC 0 device 15 function 0 on the
 * socket's uncore bus carries two controllers, STATUS/COMMAND/CONTROL at
 * 0x180/0x184/0x188 and 0x190/0x194/0x198. Those offsets are above 0xff, so
 * they need ECAM. Model evidence only: not yet verified on X79-VG4 hardware.
 *
 * Read-only by construction: the only command built is a byte read, and
 * the write bit cannot be produced by any caller. The control register is
 * written back exactly as found after every byte, which also gives the
 * firmware back its DIMM thermal-sensor polling. */
#include <stddef.h>
#include "intel_imc_smbus.h"
#include "pci.h"
#include "ktime.h"
#include "kprintf.h"

#define INTEL_VENDOR_ID     0x8086u
#define IMC_SLOT            15u
#define IMC_FUNC            0u

#define IMC_STATUS(c)       (0x180u + 0x10u * (c))
#define IMC_COMMAND(c)      (0x184u + 0x10u * (c))
#define IMC_CONTROL(c)      (0x188u + 0x10u * (c))

#define STATUS_BUSY         0x10000000u
#define STATUS_BUS_ERROR    0x20000000u
#define STATUS_READ_VALID   0x80000000u
#define CMD_WRITE           0x08000000u
#define CMD_TRIGGER         0x80000000u
#define CNTL_POLL_EN        0x00000100u
#define CNTL_CLK_OVERRIDE   0x08000000u
#define CNTL_WRITE_DISABLE  0x04000000u
#define CNTL_DTI_MASK       0xf0000000u

#define SETTLE_NS          40000000ull  /* FreeBSD: 4 x 10 ms */
#define XFER_NS            35000000ull  /* SMBus maximum transaction time */
#define POLL_LIMIT         1000000u

#define SPD_FIRST_ADDR 0x50u
#define SPD_LAST_ADDR  0x57u

enum { XFER_OK = 0, XFER_NACK = -1, XFER_TIMEOUT = -2, XFER_STUCK = -3 };

static const struct { uint16_t id; const char *name; } imc_ids[] = {
    { 0x3ca8, "Sandy Bridge-EP" },  /* FreeBSD IMCSMB_PCI_DEV_ID_IMC0_SBX */
    { 0x0ea8, "Ivy Bridge-EP" },    /* FreeBSD IMCSMB_PCI_DEV_ID_IMC0_IBX */
};

static struct imc_spd_inventory inventory;
static int scanned;

const struct imc_spd_inventory *imc_spd_inventory(void) { return &inventory; }

/* The only command this file can issue. The 7-bit address splits into the
 * DTI (control [31:28]) and the slave select (command [26:24]). */
static uint32_t read_command(uint8_t addr, uint8_t offset)
{
    return CMD_TRIGGER | (uint32_t)(addr & 7u) << 24 | (uint32_t)offset << 16;
}

static int byte_read(uint8_t bus, unsigned c, uint8_t addr, uint8_t offset, uint8_t *out)
{
    uint32_t orig = pci_cfg_read(bus, IMC_SLOT, IMC_FUNC, IMC_CONTROL(c));
    /* all ones: the offset is outside what this bus's config path decodes */
    if (orig == UINT32_MAX) return XFER_STUCK;
    /* FreeBSD: CLK_OVERRIDE for normal operation, POLL_EN off so the
     * hardware TSOD poll is not mid-transaction, and WRITE_DISABLE clear
     * ("the datasheet says this isn't necessary, but empirically, it is").
     * The command below never carries CMD_WRITE, so no write can result. */
    uint32_t cntl = (orig & ~(CNTL_DTI_MASK | CNTL_POLL_EN | CNTL_WRITE_DISABLE)) |
                    (uint32_t)(addr >> 3) << 28 | CNTL_CLK_OVERRIDE;
    pci_cfg_write(bus, IMC_SLOT, IMC_FUNC, IMC_CONTROL(c), cntl);

    int rc = XFER_STUCK;
    uint32_t s = 0;
    unsigned polls = 0;
    uint64_t deadline = time_mono_raw_ns() + SETTLE_NS;
    do {
        s = pci_cfg_read(bus, IMC_SLOT, IMC_FUNC, IMC_STATUS(c));
        if (!(s & STATUS_BUSY)) { rc = XFER_OK; break; }
    } while (++polls < POLL_LIMIT && time_mono_raw_ns() < deadline);

    if (rc == XFER_OK) {
        uint32_t cmd = read_command(addr, offset);
#ifndef IMC_NEGCTL_WRITE_BIT
        cmd &= ~CMD_WRITE;
#else
        cmd |= CMD_WRITE;
#endif
        pci_cfg_write(bus, IMC_SLOT, IMC_FUNC, IMC_COMMAND(c), cmd);
        rc = XFER_TIMEOUT;
        polls = 0;
        deadline = time_mono_raw_ns() + XFER_NS;
        do {
            s = pci_cfg_read(bus, IMC_SLOT, IMC_FUNC, IMC_STATUS(c));
            if (s & STATUS_READ_VALID) { rc = XFER_OK; break; }
        } while (++polls < POLL_LIMIT && time_mono_raw_ns() < deadline);
        if (rc == XFER_OK && (s & STATUS_BUS_ERROR)) rc = XFER_NACK;
        if (rc == XFER_OK) *out = (uint8_t)s;
    }
#ifndef IMC_NEGCTL_NO_RESTORE
    pci_cfg_write(bus, IMC_SLOT, IMC_FUNC, IMC_CONTROL(c), orig);
#endif
    return rc;
}

static const char *xfer_name(int rc)
{
    return rc == XFER_TIMEOUT ? "timeout" : rc == XFER_STUCK ? "busy" : "nack";
}

static void scan_controller(uint8_t bus, unsigned c)
{
    uint8_t spd[DDR3_SPD_BYTES_NEEDED];
    for (uint8_t addr = SPD_FIRST_ADDR; addr <= SPD_LAST_ADDR; addr++) {
        int rc = byte_read(bus, c, addr, 2, &spd[2]);
        if (rc == XFER_NACK) continue;
        if (rc != XFER_OK) {
            inventory.read_errors++;
            kprintf("[spd] imc smbus%u addr=0x%02x %s, controller skipped\n", c, addr, xfer_name(rc));
            return;
        }
        if (spd[2] != 0x0b) {
            inventory.rejected++;
            kprintf("[spd] imc smbus%u addr=0x%02x type=0x%02x rejected=not-ddr3\n", c, addr, spd[2]);
            continue;
        }
        for (unsigned off = 0; off < DDR3_SPD_BYTES_NEEDED && rc == XFER_OK; off++)
            rc = byte_read(bus, c, addr, (uint8_t)off, &spd[off]);
        if (rc != XFER_OK) {
            inventory.read_errors++;
            kprintf("[spd] imc smbus%u addr=0x%02x %s mid-read\n", c, addr, xfer_name(rc));
            if (rc != XFER_NACK) return;
            continue;
        }
        struct ddr3_spd_info info;
        rc = ddr3_spd_decode(spd, &info);
        if (rc != DDR3_SPD_OK) {
            inventory.rejected++;
            kprintf("[spd] imc smbus%u addr=0x%02x rejected=%s\n", c, addr,
                    rc == DDR3_SPD_BAD_CRC ? "bad-crc" :
                    rc == DDR3_SPD_BAD_GEOMETRY ? "bad-geometry" : "not-ddr3");
            continue;
        }
        if (inventory.modules >= IMC_SPD_MAX_MODULES) return;
        struct imc_spd_module *m = &inventory.module[inventory.modules++];
        m->controller = (uint8_t)c;
        m->address = addr;
        m->ddr3 = info;
        inventory.module_capacity_mib += info.module_mib;
        kprintf("[spd] imc smbus%u addr=0x%02x ddr3 %u MiB ranks=%u x%u bus=%u ecc=%u "
                "mfg=%04x serial=%02x%02x%02x%02x part=%s\n",
                c, addr, info.module_mib, info.ranks, info.device_width, info.bus_width,
                info.ecc_bits, info.manufacturer_raw, info.serial[0], info.serial[1],
                info.serial[2], info.serial[3], info.part_number[0] ? info.part_number : "-");
    }
}

void imc_spd_scan(void)
{
    if (scanned) return;
    scanned = 1;
    if (!pci_ecam_active()) {
        kprintf("[spd] imc: no ECAM, iMC SMBus registers (0x180+) unreachable\n");
        return;
    }
    /* The uncore bus is not behind any bridge, so enumeration never sees it.
     * Socket 0's is the highest bus of its range: 0xff on a one-socket board
     * whose MCFG spans every bus, lower when firmware splits the range. */
    const char *name = NULL;
    for (int bus = 255; bus >= 0 && !name; bus--) {
        uint32_t id = pci_cfg_read((uint8_t)bus, IMC_SLOT, IMC_FUNC, 0);
        if ((id & 0xffffu) != INTEL_VENDOR_ID) continue;
        for (unsigned i = 0; i < sizeof imc_ids / sizeof imc_ids[0]; i++)
            if ((id >> 16) == imc_ids[i].id) {
                name = imc_ids[i].name;
                inventory.device = imc_ids[i].id;
                inventory.bus = (uint8_t)bus;
            }
    }
    if (!name) {
        kprintf("[spd] imc: no Sandy/Ivy Bridge-EP iMC 0 found\n");
        return;
    }
    kprintf("[spd] imc %s iMC 0 8086:%04x at %02x:0f.0, SPD read-only 0x50-0x57\n",
            name, inventory.device, inventory.bus);
    for (unsigned c = 0; c < 2; c++) scan_controller(inventory.bus, c);
    kprintf("[spd] imc modules=%u capacity_mib=%llu rejected=%u read_errors=%u\n",
            inventory.modules, (unsigned long long)inventory.module_capacity_mib,
            inventory.rejected, inventory.read_errors);
}
