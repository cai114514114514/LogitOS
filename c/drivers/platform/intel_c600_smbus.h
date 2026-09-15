/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_INTEL_C600_SMBUS_H
#define LOGIT_INTEL_C600_SMBUS_H

#include <stdint.h>
#include "ddr3_spd.h"

struct device;

#define C600_SPD_FIRST_ADDR 0x50u
#define C600_SPD_LAST_ADDR  0x57u
#define C600_SPD_MAX_MODULES 8u

struct c600_spd_module {
    uint8_t address;
    struct ddr3_spd_info ddr3;
};

struct c600_spd_inventory {
    uint16_t io_base;
    unsigned modules;
    unsigned rejected;
    unsigned read_errors;
    uint64_t module_capacity_mib;
    struct c600_spd_module module[C600_SPD_MAX_MODULES];
};

int c600_smbus_probe(struct device *dev);
const struct c600_spd_inventory *c600_spd_inventory(void);

#endif
