/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_INTEL_IMC_SMBUS_H
#define LOGIT_INTEL_IMC_SMBUS_H

#include <stdint.h>
#include "ddr3_spd.h"

#define IMC_SPD_MAX_MODULES 16u     /* 2 SMBus controllers x 0x50..0x57 */

struct imc_spd_module {
    uint8_t controller;             /* SMBus 0 or 1 of the iMC */
    uint8_t address;
    struct ddr3_spd_info ddr3;
};

struct imc_spd_inventory {
    uint16_t device;                /* iMC 0 PCI ID, 0 if none was found */
    uint8_t bus;                    /* the socket's uncore bus */
    unsigned modules;
    unsigned rejected;
    unsigned read_errors;
    uint64_t module_capacity_mib;
    struct imc_spd_module module[IMC_SPD_MAX_MODULES];
};

/* Sandy/Ivy Bridge-EP put the DIMMs' SPD EEPROMs on the CPU's own iMC SMBus,
 * not on the PCH SMBus (X79-VG4: the PCH scan finds no modules). Find socket
 * 0's iMC 0 on its uncore bus and read every SPD there. Read-only; runs once. */
void imc_spd_scan(void);
const struct imc_spd_inventory *imc_spd_inventory(void);

#endif
