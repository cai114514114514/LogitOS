/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Intel C600/X79 host SMBus, deliberately limited to read-only DDR3 SPD
 * discovery.  PCI class 0c:05 says "SMBus" but does not standardize this I/O
 * register layout, so binding every controller with that class would be a
 * hardware-corruption risk.  Intel's specification update identifies the
 * C600/X79 D31:F3 function as 8086:1d22; that exact ID plus the class is the
 * compatibility boundary here.
 *
 * No public arbitrary-address API exists.  The only issued bus operation is
 * SMBus Byte Data READ to 0x50..0x57.  In particular there is no Quick command
 * probe and no write direction, because either can modify a poorly protected
 * EEPROM on real hardware. */
#include "intel_c600_smbus.h"
#include "driver.h"
#include "pci.h"
#include "io.h"
#include "ktime.h"
#include "kprintf.h"

#define INTEL_VENDOR_ID 0x8086u
#define C600_SMBUS_ID   0x1d22u
#define SMBUS_CLASS     0x0cu
#define SMBUS_SUBCLASS  0x05u
#define SMB_BASE_BAR    4
#define SMB_HOSTC       0x40u
#define HOSTC_HST_EN    0x01u
#define HOSTC_I2C_EN    0x04u

#define HST_STS         0x00u
#define HST_CNT         0x02u
#define HST_CMD         0x03u
#define XMIT_SLVA       0x04u
#define HST_D0          0x05u
#define STS_HOST_BUSY   0x01u
#define STS_INTR        0x02u
#define STS_DEV_ERR     0x04u
#define STS_BUS_ERR     0x08u
#define STS_FAILED      0x10u
#define STS_INUSE       0x40u
#define STS_TERMINAL    (STS_INTR | STS_DEV_ERR | STS_BUS_ERR | STS_FAILED)
#define CNT_KILL        0x02u
#define CNT_BYTE_DATA   0x08u
#define CNT_START       0x40u

#define CLAIM_NS        5000000ull
#define IDLE_NS        35000000ull
#define XFER_NS        35000000ull
#define KILL_NS         5000000ull
#define POLL_LIMIT      100000u

#ifndef X79_SPD_READ_BIT
#define X79_SPD_READ_BIT 1u
#endif

enum xfer_result { XFER_OK = 0, XFER_NO_DEVICE = -1, XFER_COLLISION = -2,
                   XFER_TIMEOUT = -3, XFER_BUSY = -4,
                   XFER_QUARANTINED = -5 };

static struct c600_spd_inventory inventory;
/* This exact device ID represents the single PCH SMBus function.  Once a
 * failed recovery leaves either its transaction engine or PCI decoder state
 * unknowable, retrying the probe would turn a bounded diagnostic read into an
 * unowned port access.  Keep the refusal for the lifetime of this boot. */
static int controller_quarantined;

const struct c600_spd_inventory *c600_spd_inventory(void) { return &inventory; }

static int before_deadline(uint64_t deadline, unsigned *polls)
{
    if ((*polls)++ >= POLL_LIMIT) return 0;
    return time_mono_raw_ns() < deadline;
}

/* Reading INUSE when it is clear atomically claims the hardware semaphore;
 * writing one releases it.  Never clear bit 6 while polling somebody else's
 * ownership, and never KILL a transaction that this driver did not start. */
static int claim(uint16_t io)
{
    uint64_t deadline = time_mono_raw_ns() + CLAIM_NS;
    unsigned polls = 0;
    do {
        if (!(inb((uint16_t)(io + HST_STS)) & STS_INUSE)) return 0;
    } while (before_deadline(deadline, &polls));
    return -1;
}

static void release(uint16_t io) { outb((uint16_t)(io + HST_STS), STS_INUSE); }

static int wait_not_busy(uint16_t io, uint64_t ns, uint8_t *last)
{
    uint64_t deadline = time_mono_raw_ns() + ns;
    unsigned polls = 0;
    do {
        uint8_t s = inb((uint16_t)(io + HST_STS));
        if (last) *last = s;
        if (!(s & STS_HOST_BUSY)) return 0;
    } while (before_deadline(deadline, &polls));
    return -1;
}

static int byte_data_read(uint16_t io, uint8_t addr, uint8_t command, uint8_t *value)
{
    if (controller_quarantined) return XFER_QUARANTINED;
    if (claim(io) != 0) return XFER_BUSY;
    uint8_t status = 0;
    if (wait_not_busy(io, IDLE_NS, &status) != 0) {
        release(io);
        return XFER_BUSY;
    }

    /* Clear only command-completion status.  SMBALERT belongs to firmware or
     * another consumer and INUSE is our semaphore, so neither is in this mask. */
    outb((uint16_t)(io + HST_STS), STS_TERMINAL);
    outb((uint16_t)(io + HST_CMD), command);
    outb((uint16_t)(io + XMIT_SLVA),
         (uint8_t)((addr << 1) | (X79_SPD_READ_BIT & 1u)));
    outb((uint16_t)(io + HST_CNT), CNT_BYTE_DATA | CNT_START);

    uint64_t deadline = time_mono_raw_ns() + XFER_NS;
    unsigned polls = 0;
    for (;;) {
        status = inb((uint16_t)(io + HST_STS));
        if (!(status & STS_HOST_BUSY) && (status & STS_TERMINAL)) break;
        if (!before_deadline(deadline, &polls)) {
#ifndef X79_SMBUS_NEG_NO_KILL
            outb((uint16_t)(io + HST_CNT), CNT_KILL);
            int kill_rc = wait_not_busy(io, KILL_NS, &status);
#ifndef X79_SMBUS_NEG_SKIP_KILL_VERIFY
            /* Clearing KILL or releasing INUSE while BUSY is still asserted
             * would let a later owner race the transaction we failed to stop.
             * Retain both the asserted KILL and our semaphore, and permanently
             * refuse this controller for the remainder of the boot. */
            if (kill_rc != 0) {
                controller_quarantined = 1;
                kprintf("[smbus] io=0x%x KILL left BUSY set; "
                        "controller quarantined, semaphore retained\n", io);
                return XFER_QUARANTINED;
            }
#else
            (void)kill_rc;
#endif
            outb((uint16_t)(io + HST_CNT), 0); /* KILL must be cleared. */
#endif
            outb((uint16_t)(io + HST_STS), STS_TERMINAL);
            release(io);
            return XFER_TIMEOUT;
        }
    }

    int rc = XFER_OK;
    if (status & STS_DEV_ERR) rc = XFER_NO_DEVICE;
    else if (status & (STS_BUS_ERR | STS_FAILED)) rc = XFER_COLLISION;
    else if (!(status & STS_INTR)) rc = XFER_TIMEOUT;
    else if (value) *value = inb((uint16_t)(io + HST_D0));
    outb((uint16_t)(io + HST_STS), STS_TERMINAL);
    release(io);
    return rc;
}

static int restore_command(struct device *dev, uint16_t old_cmd)
{
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, old_cmd);
#ifndef X79_SMBUS_NEG_SKIP_CMD_RESTORE_VERIFY
    uint16_t restored = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                       PCI_CFG_COMMAND);
    if (restored != old_cmd) {
        /* No BAR access has happened yet.  Preserve that boundary after an
         * ignored recovery write by making every later probe refuse before it
         * can touch the I/O window whose decode ownership is now uncertain. */
        controller_quarantined = 1;
        kprintf("[smbus] %s PCI Command restore refused old=0x%x got=0x%x; "
                "controller quarantined\n", dev->name, old_cmd, restored);
        return -1;
    }
#endif
    return 0;
}

static const char *decode_error(int rc)
{
    if (rc == DDR3_SPD_NOT_DDR3) return "not-ddr3";
    if (rc == DDR3_SPD_BAD_CRC) return "crc";
    return "geometry";
}

static void inventory_reset(uint16_t io)
{
    uint8_t *p = (uint8_t *)&inventory;
    for (unsigned i = 0; i < sizeof inventory; i++) p[i] = 0;
    inventory.io_base = io;
}

static void scan_spd(uint16_t io)
{
    uint8_t spd[DDR3_SPD_BYTES_NEEDED];
    for (uint8_t addr = C600_SPD_FIRST_ADDR; addr <= C600_SPD_LAST_ADDR; addr++) {
        int rc = byte_data_read(io, addr, 2, &spd[2]);
        if (rc == XFER_NO_DEVICE) continue;
        if (rc != XFER_OK) {
            inventory.read_errors++;
            kprintf("[spd] addr=0x%02x read-error=%d scan-aborted\n", addr, rc);
            break; /* a sick controller should not cost seven more timeouts */
        }
        /* Byte 2 identifies the SPD layout.  Do not walk 145 more command
         * offsets on an unrelated device merely because it answered at an
         * address conventionally used by DIMMs. */
        if (spd[2] != 0x0b) {
            inventory.rejected++;
            kprintf("[spd] addr=0x%02x rejected=not-ddr3\n", addr);
            continue;
        }
        for (unsigned off = 0; off < DDR3_SPD_BYTES_NEEDED; off++) {
            rc = byte_data_read(io, addr, (uint8_t)off, &spd[off]);
            if (rc != XFER_OK) break;
        }
        if (rc != XFER_OK) {
            inventory.read_errors++;
            kprintf("[spd] addr=0x%02x read-error=%d byte-scan\n", addr, rc);
            if (rc == XFER_TIMEOUT || rc == XFER_BUSY) break;
            continue;
        }
        struct ddr3_spd_info info;
        rc = ddr3_spd_decode(spd, &info);
        if (rc != DDR3_SPD_OK) {
            inventory.rejected++;
            kprintf("[spd] addr=0x%02x rejected=%s\n", addr, decode_error(rc));
            continue;
        }
        struct c600_spd_module *m = &inventory.module[inventory.modules++];
        m->address = addr;
        m->ddr3 = info;
        inventory.module_capacity_mib += info.module_mib;
        kprintf("[spd] addr=0x%02x ddr3 module_mib=%u ranks=%u device_width=%u "
                "bus_width=%u ecc_bits=%u crc=ok mfg_raw=%04x "
                "serial=%02x%02x%02x%02x part=%s\n",
                addr, info.module_mib, info.ranks, info.device_width,
                info.bus_width, info.ecc_bits, info.manufacturer_raw,
                info.serial[0], info.serial[1], info.serial[2], info.serial[3],
                info.part_number[0] ? info.part_number : "-");
    }
    kprintf("[spd] modules=%u module_capacity_mib=%llu rejected=%u read_errors=%u "
            "source=SPD-not-firmware-map\n", inventory.modules,
            (unsigned long long)inventory.module_capacity_mib,
            inventory.rejected, inventory.read_errors);
}

int c600_smbus_probe(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI || dev->vendor != INTEL_VENDOR_ID ||
        dev->device != C600_SMBUS_ID || dev->class_code != SMBUS_CLASS ||
        dev->subclass != SMBUS_SUBCLASS)
        return -1;
    if (controller_quarantined) {
        kprintf("[smbus] %s controller quarantined; reprobe refused\n", dev->name);
        return -1;
    }
    if (!time_ready()) return -1;
    struct dev_resource *bar = &dev->res[SMB_BASE_BAR];
    if (!(bar->flags & DEV_RES_IO) || !bar->start || bar->start > 0xffe0u ||
        bar->size < 0x20u || (bar->start & 0x1fu)) {
        kprintf("[smbus] %s C600/X79 unusable I/O BAR\n", dev->name);
        return -1;
    }

    uint8_t hostc = pci_cfg_read8(dev->bus, dev->slot, dev->func, SMB_HOSTC);
    if (hostc & HOSTC_I2C_EN) {
        kprintf("[smbus] %s C600/X79 I2C mode refused\n", dev->name);
        return -1;
    }
    uint16_t old_cmd = pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
    /* dev_enable() would enable both I/O and memory decode.  This driver uses
     * only BAR4; SMBMBAR may be unconfigured, so enabling its decoder would
     * expose an address range we never validated.  A native 16-bit Command
     * write preserves Status and every pre-existing MEM/MASTER decision. */
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND,
                    (uint16_t)(old_cmd | PCI_CMD_IO));
#ifndef X79_SMBUS_NEG_SKIP_CMD_VERIFY
    /* Some firmware can lock PCI Command.  Never touch BAR4 until readback
     * proves that this function actually owns the decoded I/O aperture: an
     * ignored write could otherwise target an unrelated legacy decoder. */
    if (!(pci_cfg_read16(dev->bus, dev->slot, dev->func,
                         PCI_CFG_COMMAND) & PCI_CMD_IO)) {
        (void)restore_command(dev, old_cmd);
        kprintf("[smbus] %s C600/X79 I/O decode enable refused\n", dev->name);
        return -1;
    }
#endif
    if (!(hostc & HOSTC_HST_EN)) {
        pci_cfg_write8(dev->bus, dev->slot, dev->func, SMB_HOSTC,
                       (uint8_t)(hostc | HOSTC_HST_EN));
        if (!(pci_cfg_read8(dev->bus, dev->slot, dev->func, SMB_HOSTC) & HOSTC_HST_EN)) {
            (void)restore_command(dev, old_cmd);
            kprintf("[smbus] %s C600/X79 host enable refused\n", dev->name);
            return -1;
        }
    }

    uint16_t io = (uint16_t)bar->start;
    inventory_reset(io);
    kprintf("[smbus] %s Intel C600/X79 io=0x%x SPD read-only 0x50-0x57\n",
            dev->name, io);
    scan_spd(io);
    dev_set_drvdata(dev, &inventory);
    return 0;
}

static const struct dev_match c600_smbus_ids[] = {
    { INTEL_VENDOR_ID, C600_SMBUS_ID, SMBUS_CLASS, SMBUS_SUBCLASS, DEV_ANYC, 0 },
    DEV_MATCH_END
};
static struct driver c600_smbus_driver = {
    .name = "c600-smbus", .bus_type = DEV_BUS_PCI,
    .match = c600_smbus_ids, .probe = c600_smbus_probe,
};
DRIVER_DECLARE(c600_smbus_driver);
