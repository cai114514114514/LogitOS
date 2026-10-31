/* See nct6779.h.
 *
 * Access, as Linux nct6775-platform.c: the HWM block sits at the I/O base in
 * logical device 0x0B (config 0x60/0x61, enable 0x30); its index port is
 * base+5 and data port base+6. Register 0x4E selects the bank, and a 16-bit
 * register number is bank<<8 | index. The only writes are to the index port
 * and to the bank register, which is put back as found. Correction: restoring
 * bank alone never protected against SMM changing the index between writes.
 * Refuse ACPI-owned windows before permitting any HWM transaction. */
#include <stddef.h>
#include "nct6779.h"
#include "board_io.h"
#include "nct_acpi.h"
#include "../../kernel/cpu/spinlock.h"

/* Linux v6.12 nct6775-platform.c: SIO_REG_*, NCT6775_LD_HWM,
 * IOREGION_ALIGNMENT/OFFSET; nct6775.h: NCT6775_REG_BANK. */
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
static spinlock_t nct_lock = SPINLOCK_INIT;
/* Negative errno values without the ring-3 libc errno header. Pending is the
 * boot default; AML failure leaves it there and performs no HWM I/O. */
static int access_state = -11; /* EAGAIN */

/* Configuration reads still select an index; never write a configuration
 * payload here. Linux v6.12 nct6775-platform.c: superio_inb(). */
static uint8_t sio_rd(uint16_t sio, uint8_t reg)
{
    bio_outb(sio, reg);
    return bio_inb((uint16_t)(sio + 1));
}

/* Discover without enabling or reprogramming HWM. CR07 is shared state even
 * when rejecting a device: leave the caller's logical device selected. */
int nct6779_find(uint16_t sio)
{
    uint64_t flags = spin_lock_irqsave(&nct_lock);
    int result = -1;
    /* Discovery normally happens only at boot. If it is repeated, no newly
     * selected base may inherit old permission; a firmware refusal, however,
     * remains permanent even if the repeated discovery fails. */
    if (access_state == 0) {
        access_state = -11;
    }
    g_base = 0;
    /* Linux v6.12 nct6775-platform.c: superio_enter(), two unlock keys. */
    bio_outb(sio, 0x87);
    bio_outb(sio, 0x87);
    uint8_t saved_ldn = sio_rd(sio, SIO_LDSEL);
    /* Linux v6.12 nct6775-platform.c: SIO_REG_DEVID is CR20/21;
     * SIO_NCT6779_ID is 0xC560 with SIO_ID_MASK 0xFFF8 (revision bits). */
    uint16_t id = (uint16_t)(sio_rd(sio, 0x20) << 8 | sio_rd(sio, 0x21));
    bio_outb(sio, SIO_LDSEL);
    bio_outb((uint16_t)(sio + 1), LD_HWM);
    if ((id & 0xfff8) != 0xc560) {
        goto out;
    }
    uint16_t base = (uint16_t)((sio_rd(sio, SIO_ADDR) << 8 | sio_rd(sio, SIO_ADDR + 1)) & ~7u);
    uint8_t en = sio_rd(sio, SIO_ENABLE);
    /* Linux v6.12 nct6775-platform.c: nct6775_find() aligns CR60/61 to
     * eight bytes and tests CR30 bit 0. Linux enables a disabled HWM
     * ("sensor is probably unusable"); this read-only driver refuses it.
     * Base zero is never accepted as a device. */
#ifndef NCT_NEGCTL_IGNORE_ENABLE
    if (!(en & 1) || !base) {
        goto out;
    }
#else
    (void)en;
    if (!base) {
        goto out;
    }
#endif
    g_base = base;
    result = 0;
out:
    /* Previously CR07 remained 0x0B on success and both rejection paths.
     * Restore before superio_exit()'s 0xAA key, while CR07 is accessible. */
#ifndef NCT_NEGCTL_NO_LDN_RESTORE
    bio_outb(sio, SIO_LDSEL);
    bio_outb((uint16_t)(sio + 1), saved_ldn);
#else
    (void)saved_ldn;
#endif
    bio_outb(sio, 0xaa);
    spin_unlock_irqrestore(&nct_lock, flags);
    return result;
}

/* Publish permission only after the sleepable namespace scan. Discovery runs
 * before this boot-only call; a changed base during a scan fails closed. No
 * spinlock can exclude SMM, so a firmware claim is a permanent refusal.
 * Linux v6.12 nct6775-platform.c: sensors_nct6775_platform_init(). */
int nct6779_acpi_gate(void)
{
    uint64_t flags = spin_lock_irqsave(&nct_lock);
    uint16_t base = g_base;
    int state = access_state;
    spin_unlock_irqrestore(&nct_lock, flags);
    if (state != -11) {
        return state;
    }
    if (!base) {
        return -1;
    }

    /* Correction: checking only base+5/+6 would already catch a whole-device
     * OperationRegion claim. Checking the full aligned eight-byte window also
     * rejects firmware claims covering only its other bytes, as required by
     * P0's HWM access policy. */
#ifndef NCT_NEGCTL_NO_ACPI_GATE
    state = nct_acpi_check(base, 8);
#else
    state = 0;
#endif
    flags = spin_lock_irqsave(&nct_lock);
    if (access_state == -11) {
        /* Keep the scan's EIO distinct from a firmware claimant's EBUSY.
         * A base changed during the scan is also unverified, not owned. */
        access_state = g_base == base ? state : -5; /* EIO */
    }
    state = access_state;
    spin_unlock_irqrestore(&nct_lock, flags);
    return state;
}

/* Read a banked byte only while the snapshot owns nct_lock and the ACPI gate
 * is open. Correction: the previous nct6775_read_value() citation did not
 * identify the port-access implementation. Linux v6.12 nct6775-platform.c:
 * nct6775_set_bank() selects the bank; nct6775_reg_read() reads the byte.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/hwmon/nct6775-platform.c */
static uint8_t hwm_rd(uint16_t reg)
{
    bio_outb((uint16_t)(g_base + ADDR_PORT), REG_BANK);
    bio_outb((uint16_t)(g_base + DATA_PORT), (uint8_t)(reg >> 8));
    bio_outb((uint16_t)(g_base + ADDR_PORT), (uint8_t)reg);
    return bio_inb((uint16_t)(g_base + DATA_PORT));
}

/* Keep the saved bank inside the lock: two readers otherwise save each other's
 * temporary bank 4 and silently restore the wrong firmware state. This lock
 * coordinates OS readers only; it is not claimed to exclude SMM. */
int nct6779_read(struct nct_sensors *out)
{
#ifndef NCT_NEGCTL_NO_LOCK
    uint64_t flags = spin_lock_irqsave(&nct_lock);
#endif
    int result = access_state;
    out->base = g_base;
    /* A later failed discovery must not erase a permanent admission error. */
    if (!g_base && (result == 0 || result == -11)) {
        result = -1;
    }
    if (result) {
        goto out;
    }
    bio_outb((uint16_t)(g_base + ADDR_PORT), REG_BANK);
    uint8_t bank = bio_inb((uint16_t)(g_base + DATA_PORT));
    for (int i = 0; i < NCT_TEMPS; i++) {
        out->temp_c[i] = (int8_t)hwm_rd(temp_reg[i]);
    }
    for (int i = 0; i < NCT_FANS; i++) {
        out->fan_rpm[i] = (uint16_t)(hwm_rd(fan_reg[i]) << 8 | hwm_rd((uint16_t)(fan_reg[i] + 1)));
    }
    for (int i = 0; i < NCT_VOLTS; i++) {
        out->mv[i] = (uint16_t)((hwm_rd((uint16_t)(0x480 + i)) * volt_scale[i] + 50) / 100);
    }
#ifndef NCT_NEGCTL_KEEP_BANK
    bio_outb((uint16_t)(g_base + ADDR_PORT), REG_BANK);
    bio_outb((uint16_t)(g_base + DATA_PORT), bank);
#else
    (void)bank;
#endif
out:
#ifndef NCT_NEGCTL_NO_LOCK
    spin_unlock_irqrestore(&nct_lock, flags);
#endif
    return result;
}
