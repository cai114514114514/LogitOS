/* See nct6779.h.
 *
 * Access, as Linux nct6775-platform.c: the HWM block sits at the I/O base in
 * logical device 0x0B (config 0x60/0x61, enable 0x30); its index port is
 * base+5 and data port base+6. Register 0x4E selects the bank, and a 16-bit
 * register number is bank<<8 | index. The only writes are to the index port
 * and to the bank register, which is put back as found: firmware may be
 * reading the same block from SMM. */
#include <stddef.h>
#include "nct6779.h"
#include "board_io.h"

#define LD_HWM      0x0b
#define SIO_LDSEL   0x07
#define SIO_ENABLE  0x30
#define SIO_ADDR    0x60
#define REG_BANK    0x4e
#define ADDR_PORT   5
#define DATA_PORT   6

const char *const nct_temp_label[NCT_TEMPS] = {
    "SYSTIN", "CPUTIN", "AUXTIN0", "AUXTIN1", "AUXTIN2", "AUXTIN3" };
const char *const nct_volt_label[NCT_VOLTS] = {
    "CPUVC", "VIN1", "AVSB", "3VCC", "VIN0", "VIN8", "VIN4", "3VSB",
    "VBAT", "VTT", "VIN5", "VIN6", "VIN2", "VIN3", "VIN7" };

/* NCT6779_REG_TEMP_ALTERNATE[src-1] for sources 1-6 (byte, integer C) */
static const uint16_t temp_reg[NCT_TEMPS] = { 0x490, 0x491, 0x492, 0x493, 0x494, 0x495 };
/* NCT6779_REG_FAN: word-sized, the value is the RPM (fan_from_reg_rpm) */
static const uint16_t fan_reg[NCT_FANS] = { 0x4c0, 0x4c2, 0x4c4, 0x4c6, 0x4c8, 0x4ca };
/* NCT6779_REG_IN 0x480-0x48e; scale_in: 8 mV LSB times the internal divider, x100 */
static const uint16_t volt_scale[NCT_VOLTS] = {
    800, 800, 1600, 1600, 800, 800, 800, 1600, 1600, 800, 800, 800, 800, 800, 800 };

static uint16_t g_base;

static uint8_t sio_rd(uint16_t sio, uint8_t reg) { bio_outb(sio, reg); return bio_inb((uint16_t)(sio + 1)); }

int nct6779_find(uint16_t sio)
{
    g_base = 0;
    bio_outb(sio, 0x87); bio_outb(sio, 0x87);           /* enter configuration */
    uint16_t id = (uint16_t)(sio_rd(sio, 0x20) << 8 | sio_rd(sio, 0x21));
    bio_outb(sio, SIO_LDSEL); bio_outb((uint16_t)(sio + 1), LD_HWM);
    uint16_t base = (uint16_t)((sio_rd(sio, SIO_ADDR) << 8 | sio_rd(sio, SIO_ADDR + 1)) & ~7u);
    uint8_t en = sio_rd(sio, SIO_ENABLE);
    bio_outb(sio, 0xaa);                                /* leave */
    if ((id & 0xfff8) != 0xc560) return -1;             /* SIO_NCT6779_ID, SIO_ID_MASK */
    /* Linux enables a disabled HWM ("sensor is probably unusable"); a
     * read-only driver reports it instead. Base 0 is never a device. */
#ifndef NCT_NEGCTL_IGNORE_ENABLE
    if (!(en & 1) || !base) return -1;
#else
    (void)en;
    if (!base) return -1;
#endif
    g_base = base;
    return 0;
}

static uint8_t hwm_rd(uint16_t reg)
{
    bio_outb((uint16_t)(g_base + ADDR_PORT), REG_BANK);
    bio_outb((uint16_t)(g_base + DATA_PORT), (uint8_t)(reg >> 8));
    bio_outb((uint16_t)(g_base + ADDR_PORT), (uint8_t)reg);
    return bio_inb((uint16_t)(g_base + DATA_PORT));
}

int nct6779_read(struct nct_sensors *out)
{
    out->base = g_base;
    if (!g_base) return -1;
    bio_outb((uint16_t)(g_base + ADDR_PORT), REG_BANK);
    uint8_t bank = bio_inb((uint16_t)(g_base + DATA_PORT));
    for (int i = 0; i < NCT_TEMPS; i++) out->temp_c[i] = (int8_t)hwm_rd(temp_reg[i]);
    for (int i = 0; i < NCT_FANS; i++)
        out->fan_rpm[i] = (uint16_t)(hwm_rd(fan_reg[i]) << 8 | hwm_rd((uint16_t)(fan_reg[i] + 1)));
    for (int i = 0; i < NCT_VOLTS; i++)
        out->mv[i] = (uint16_t)((hwm_rd((uint16_t)(0x480 + i)) * volt_scale[i] + 50) / 100);
#ifndef NCT_NEGCTL_KEEP_BANK
    bio_outb((uint16_t)(g_base + ADDR_PORT), REG_BANK);
    bio_outb((uint16_t)(g_base + DATA_PORT), bank);
#else
    (void)bank;
#endif
    return 0;
}
