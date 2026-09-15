/* SPDX-License-Identifier: MIT */
/* Register-accurate enough C600 SMBus model to prove that production code
 * issues only bounded Byte Data reads and preserves unrelated PCI state. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver.h"
#include "pci.h"
#include "ddr3_spd.h"
#include "intel_c600_smbus.h"

#define IO_BASE 0x500u
#define HST_STS 0x00u
#define HST_CNT 0x02u
#define HST_CMD 0x03u
#define XMIT_SLVA 0x04u
#define HST_D0 0x05u
#define STS_BUSY 0x01u
#define STS_INTR 0x02u
#define STS_DEVERR 0x04u
#define STS_FAILED 0x10u
#define STS_INUSE 0x40u
#define CNT_KILL 0x02u
#define CNT_BYTE_DATA 0x08u
#define CNT_START 0x40u

static unsigned checks, fails;
#define CHECK(x, msg) do { checks++; if (!(x)) { fails++; printf("FAIL: %s\n", msg); } } while (0)

struct model {
    uint8_t reg[32];
    uint8_t spd[8][DDR3_SPD_BYTES_NEEDED];
    uint8_t present[8];
    uint8_t status;
    int inuse;
    int external_inuse;
    int stuck_after_start;
    int busy_before_start;
    int kill_write_ignored;
    int hostc_write_ignored;
    int command_write_ignored;
    int command_restore_write_ignored;
    unsigned starts, kill_writes, kill_clears, releases;
    unsigned starts_by_addr[8];
    unsigned write_directions, wrong_protocol, outside_spd;
    unsigned time_reads, io_reads, io_writes;
} hw;
static uint16_t cfg_command, cfg_status;
static uint8_t cfg_hostc;
static int clock_ready = 1;

static uint16_t crc16(const uint8_t *p, unsigned n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1);
    }
    return crc;
}

static void make_spd(uint8_t *p, int short_crc, unsigned density_code,
                     unsigned ranks, unsigned module_type, const char *part)
{
    memset(p, 0, DDR3_SPD_BYTES_NEEDED);
    p[0] = short_crc ? 0x80 : 0x00;
    p[1] = 0x11; p[2] = 0x0b; p[3] = (uint8_t)module_type;
    p[4] = (uint8_t)density_code;
    p[7] = (uint8_t)(((ranks - 1u) << 3) | 1u); /* x8 devices */
    p[8] = 0x0b;                                /* 64 data + 8 ECC */
    p[117] = 0x80; p[118] = 0x2c;
    p[122] = 0x12; p[123] = 0x34; p[124] = 0x56; p[125] = 0x78;
    memset(p + 128, ' ', 18);
    size_t n = strlen(part); if (n > 18) n = 18;
    memcpy(p + 128, part, n);
    uint16_t crc = crc16(p, short_crc ? 117u : 126u);
    p[126] = (uint8_t)crc; p[127] = (uint8_t)(crc >> 8);
}

static void reset_model(void)
{
    memset(&hw, 0, sizeof hw);
    cfg_command = PCI_CMD_MEM | PCI_CMD_MASTER;
    cfg_status = 0xa55a;
    cfg_hostc = 0x80;
    clock_ready = 1;
}

uint64_t time_mono_raw_ns(void) { hw.time_reads++; return (uint64_t)hw.time_reads * 1000000ull; }
int time_ready(void) { return clock_ready; }

uint8_t inb(uint16_t port)
{
    unsigned off = (unsigned)(port - IO_BASE);
    if (off >= sizeof hw.reg) return 0xff;
    hw.io_reads++;
    if (off == HST_STS) {
        uint8_t value = hw.status;
        if (hw.busy_before_start) value |= STS_BUSY;
        if (hw.external_inuse || hw.inuse) value |= STS_INUSE;
        if (!hw.external_inuse && !hw.inuse) hw.inuse = 1; /* read-claim */
        return value;
    }
    return hw.reg[off];
}

void outb(uint16_t port, uint8_t value)
{
    unsigned off = (unsigned)(port - IO_BASE);
    if (off >= sizeof hw.reg) return;
    hw.io_writes++;
    if (off == HST_STS) {
        hw.status &= (uint8_t)~(value & 0x1eu);
        if (value & STS_INUSE) { hw.inuse = 0; hw.releases++; }
        return;
    }
    hw.reg[off] = value;
    if (off != HST_CNT) return;
    if (value == CNT_KILL) {
        hw.kill_writes++;
        if (hw.kill_write_ignored) return;
        hw.status &= (uint8_t)~STS_BUSY;
        hw.status |= STS_FAILED;
        hw.stuck_after_start = 0;
        return;
    }
    if (value == 0) { hw.kill_clears++; return; }
    if (!(value & CNT_START)) return;
    hw.starts++;
    if ((value & 0x1cu) != CNT_BYTE_DATA) hw.wrong_protocol++;
    uint8_t wire = hw.reg[XMIT_SLVA];
    if (!(wire & 1u)) hw.write_directions++;
    unsigned addr = wire >> 1;
    if (addr < C600_SPD_FIRST_ADDR || addr > C600_SPD_LAST_ADDR) {
        hw.outside_spd++; hw.status = STS_DEVERR; return;
    }
    if (hw.stuck_after_start) { hw.status = STS_BUSY; return; }
    unsigned slot = addr - C600_SPD_FIRST_ADDR;
    hw.starts_by_addr[slot]++;
    if (!hw.present[slot]) { hw.status = STS_DEVERR; return; }
    hw.reg[HST_D0] = hw.spd[slot][hw.reg[HST_CMD]];
    hw.status = STS_INTR;
}

void outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
uint16_t inw(uint16_t p) { (void)p; return 0xffff; }
void outl(uint16_t p, uint32_t v) { (void)p; (void)v; }
uint32_t inl(uint16_t p) { (void)p; return 0xffffffffu; }

uint8_t pci_cfg_read8(uint8_t b, uint8_t s, uint8_t f, uint16_t off)
{ (void)b; (void)s; (void)f; return off == 0x40 ? cfg_hostc : 0; }
uint16_t pci_cfg_read16(uint8_t b, uint8_t s, uint8_t f, uint16_t off)
{ (void)b; (void)s; (void)f; return off == PCI_CFG_COMMAND ? cfg_command : cfg_status; }
void pci_cfg_write8(uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint8_t v)
{ (void)b; (void)s; (void)f; if (off == 0x40 && !hw.hostc_write_ignored) cfg_hostc = v; }
void pci_cfg_write16(uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint16_t v)
{
    (void)b; (void)s; (void)f;
    if (off != PCI_CFG_COMMAND || hw.command_write_ignored) return;
    if (hw.command_restore_write_ignored && !(v & PCI_CMD_IO) &&
        (cfg_command & PCI_CMD_IO))
        return;
    cfg_command = v;
}

static struct device c600_device(void)
{
    struct device d; memset(&d, 0, sizeof d);
    strcpy(d.name, "0000:00:1f.3");
    d.bus_type = DEV_BUS_PCI; d.bus = 0; d.slot = 31; d.func = 3;
    d.vendor = 0x8086; d.device = 0x1d22;
    d.class_code = 0x0c; d.subclass = 0x05;
    d.res[4].start = IO_BASE; d.res[4].size = 0x20; d.res[4].flags = DEV_RES_IO;
    return d;
}

static void parser_checks(void)
{
    uint8_t spd[DDR3_SPD_BYTES_NEEDED]; struct ddr3_spd_info d;
    make_spd(spd, 0, 5, 2, 1, "X79-16G-RDIMM");
    CHECK(ddr3_spd_decode(spd, &d) == 0, "valid DDR3 SPD decodes");
    CHECK(d.module_mib == 16384 && d.ranks == 2, "16 GiB module geometry is decoded");
    CHECK(d.device_width == 8 && d.bus_width == 64 && d.ecc_bits == 8,
          "device, data-bus and ECC widths stay distinct");
    CHECK(!strcmp(d.part_number, "X79-16G-RDIMM"), "part number is sanitized and trimmed");
    make_spd(spd, 1, 4, 2, 2, "X79-8G-UDIMM");
    CHECK(ddr3_spd_decode(spd, &d) == 0 && d.module_mib == 8192,
          "byte0 bit7 selects 117-byte CRC coverage");
    spd[20] ^= 1;
    CHECK(ddr3_spd_decode(spd, &d) == DDR3_SPD_BAD_CRC, "CRC corruption is rejected");
    make_spd(spd, 0, 7, 2, 1, "BAD-DENSITY");
    CHECK(ddr3_spd_decode(spd, &d) == DDR3_SPD_BAD_GEOMETRY,
          "reserved density encoding is rejected");
    make_spd(spd, 0, 5, 2, 0, "BAD-TYPE");
    CHECK(ddr3_spd_decode(spd, &d) == DDR3_SPD_BAD_GEOMETRY,
          "reserved module type is rejected");
}

static void full_driver_checks(void)
{
    reset_model();
    hw.present[0] = hw.present[1] = hw.present[2] = hw.present[3] = 1;
    make_spd(hw.spd[0], 0, 5, 2, 1, "X79-16G-RDIMM");
    make_spd(hw.spd[1], 1, 4, 2, 2, "X79-8G-UDIMM");
    make_spd(hw.spd[2], 0, 3, 2, 2, "BAD-CRC"); hw.spd[2][40] ^= 1;
    hw.spd[3][2] = 0x0c; /* a responding DDR4 SPD-shaped device */
    struct device d = c600_device();
    CHECK(c600_smbus_probe(&d) == 0, "C600/X79 SMBus binds");
    const struct c600_spd_inventory *inv = c600_spd_inventory();
    CHECK(inv->modules == 2 && inv->rejected == 2 && inv->read_errors == 0,
          "valid, corrupt and absent SPD addresses are distinguished");
    CHECK(inv->module_capacity_mib == 24576,
          "module capacities sum independently of firmware usable RAM");
    CHECK(inv->module[0].address == 0x50 && inv->module[1].address == 0x51,
          "only standard SPD addresses are published");
    CHECK(hw.starts == 446, "each present module is read through production transactions");
    CHECK(hw.starts_by_addr[3] == 1,
          "non-DDR3 responders are rejected after the type byte");
    CHECK(hw.write_directions == 0 && hw.wrong_protocol == 0 && hw.outside_spd == 0,
          "SPD transactions remain read-only Byte Data within 0x50-0x57");
    CHECK(cfg_command == (PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER),
          "I/O decode enabled while MEM and MASTER state is preserved");
    CHECK(cfg_status == 0xa55a, "adjacent PCI Status is preserved");
    CHECK(cfg_hostc == 0x81, "HST_EN is the only Host Configuration change");
    CHECK(d.drvdata == inv, "inventory is attached to the device");
    CHECK(hw.inuse == 0 && hw.releases == hw.starts,
          "hardware semaphore is released after every transaction");

}

/* Keep refusal-path setup out of full_driver_checks so each case starts with
 * a fresh emulated controller and no result can inherit a prior semaphore. */
static void refusal_checks(void)
{
    struct device d;
    reset_model(); d = c600_device(); d.device = 0x1c22;
    CHECK(c600_smbus_probe(&d) != 0 && hw.starts == 0,
          "another Intel SMBus register layout is not guessed");
    reset_model(); d = c600_device(); d.class_code = 0x01;
    CHECK(c600_smbus_probe(&d) != 0 && hw.starts == 0, "wrong PCI class is refused");
    reset_model(); d = c600_device(); d.res[4].size = 0x10;
    CHECK(c600_smbus_probe(&d) != 0 && cfg_command == (PCI_CMD_MEM | PCI_CMD_MASTER),
          "short I/O aperture is refused before enabling decode");
    reset_model(); d = c600_device(); cfg_hostc |= 0x04;
    CHECK(c600_smbus_probe(&d) != 0 && hw.starts == 0,
          "unknown I2C formatting mode is refused");
    reset_model(); d = c600_device(); hw.hostc_write_ignored = 1;
    CHECK(c600_smbus_probe(&d) != 0 && cfg_command == (PCI_CMD_MEM | PCI_CMD_MASTER),
          "failed host-enable restores the original PCI Command");
    reset_model(); d = c600_device(); hw.command_write_ignored = 1;
    CHECK(c600_smbus_probe(&d) != 0 && hw.starts == 0 &&
          cfg_command == (PCI_CMD_MEM | PCI_CMD_MASTER),
          "ignored I/O-decode write is refused before BAR access");
    reset_model(); d = c600_device(); clock_ready = 0;
    CHECK(c600_smbus_probe(&d) != 0 && hw.starts == 0, "unready timeout clock is refused");

    reset_model(); d = c600_device(); hw.stuck_after_start = 1; hw.present[0] = 1;
    CHECK(c600_smbus_probe(&d) == 0, "timed-out SPD scan does not claim a module");
    CHECK(hw.kill_writes == 1 && hw.kill_clears == 1 && hw.inuse == 0 && hw.time_reads < 1000,
          "owned timeout is killed, cleared and bounded");

    reset_model(); d = c600_device(); hw.busy_before_start = 1;
    CHECK(c600_smbus_probe(&d) == 0 && hw.starts == 0 && hw.kill_writes == 0 && hw.inuse == 0,
          "pre-existing busy transaction is never killed");
    reset_model(); d = c600_device(); hw.external_inuse = 1;
    CHECK(c600_smbus_probe(&d) == 0 && hw.starts == 0 && hw.kill_writes == 0 && hw.releases == 0,
          "another semaphore owner is neither used nor released");
}

static void readonly_control(void)
{
    reset_model(); hw.present[0] = 1;
    make_spd(hw.spd[0], 0, 5, 2, 1, "READONLY");
    struct device d = c600_device(); (void)c600_smbus_probe(&d);
    CHECK(hw.write_directions == 0, "SPD transactions remain read-only");
}

static void crc_control(void)
{
    uint8_t spd[DDR3_SPD_BYTES_NEEDED]; struct ddr3_spd_info d;
    make_spd(spd, 1, 4, 2, 2, "CRC117");
    CHECK(ddr3_spd_decode(spd, &d) == 0,
          "byte0 bit7 selects 117-byte CRC coverage");
}

static void kill_control(void)
{
    reset_model(); hw.stuck_after_start = 1; hw.present[0] = 1;
    struct device d = c600_device(); (void)c600_smbus_probe(&d);
    CHECK(hw.kill_writes == 1 && hw.kill_clears == 1 && hw.inuse == 0,
          "owned timeout is killed, cleared and bounded");
}

static void command_control(void)
{
    reset_model(); hw.command_write_ignored = 1; hw.present[0] = 1;
    make_spd(hw.spd[0], 0, 5, 2, 1, "CMD-LOCKED");
    struct device d = c600_device();
    CHECK(c600_smbus_probe(&d) != 0 && hw.starts == 0,
          "ignored I/O-decode write is refused before BAR access");
}

static void kill_ignored_control(void)
{
    reset_model();
    hw.stuck_after_start = 1;
    hw.kill_write_ignored = 1;
    struct device d = c600_device();
    int first = c600_smbus_probe(&d);
    unsigned reads = hw.io_reads, writes = hw.io_writes;
    hw.stuck_after_start = 0;
    hw.kill_write_ignored = 0;
    int second = c600_smbus_probe(&d);
    CHECK(first == 0 && second != 0 && hw.starts == 1 &&
          hw.kill_writes == 1 && hw.kill_clears == 0 &&
          hw.inuse == 1 && hw.releases == 0 &&
          hw.io_reads == reads && hw.io_writes == writes,
          "ignored KILL permanently quarantines without clearing or releasing ownership");
}

static void command_restore_control(void)
{
    reset_model();
    hw.hostc_write_ignored = 1;
    hw.command_restore_write_ignored = 1;
    struct device d = c600_device();
    int first = c600_smbus_probe(&d);
    unsigned reads = hw.io_reads, writes = hw.io_writes;
    hw.hostc_write_ignored = 0;
    hw.command_restore_write_ignored = 0;
    int second = c600_smbus_probe(&d);
    CHECK(first != 0 && second != 0 && hw.starts == 0 &&
          cfg_command == (PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER) &&
          hw.io_reads == reads && hw.io_writes == writes,
          "unconfirmed PCI Command restore quarantines before all BAR I/O");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "full")) { parser_checks(); full_driver_checks(); refusal_checks(); }
    else if (!strcmp(argv[1], "readonly")) readonly_control();
    else if (!strcmp(argv[1], "crc")) crc_control();
    else if (!strcmp(argv[1], "kill")) kill_control();
    else if (!strcmp(argv[1], "command")) command_control();
    else if (!strcmp(argv[1], "kill-ignored")) kill_ignored_control();
    else if (!strcmp(argv[1], "command-restore")) command_restore_control();
    else return 2;
    printf("X79 chipset %s: %u checks, %u failed\n", argv[1], checks, fails);
    return fails ? 1 : 0;
}
