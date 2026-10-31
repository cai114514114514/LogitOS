/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_INTEL_PCH_SMBUS_H
#define LOGIT_INTEL_PCH_SMBUS_H

#include <stdint.h>
#include "ddr3_spd.h"

struct device;

#define PCH_SPD_FIRST_ADDR 0x50u
#define PCH_SPD_LAST_ADDR  0x57u
#define PCH_SPD_MAX_MODULES 8u

struct pch_spd_module {
    uint8_t address;
    struct ddr3_spd_info ddr3;
};

struct pch_spd_inventory {
    uint16_t io_base;
    unsigned modules;
    unsigned rejected;
    unsigned read_errors;
    uint64_t module_capacity_mib;
    struct pch_spd_module module[PCH_SPD_MAX_MODULES];
};

int pch_smbus_probe(struct device *dev);
const struct pch_spd_inventory *pch_spd_inventory(void);

#endif
