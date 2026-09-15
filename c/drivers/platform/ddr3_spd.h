/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_DDR3_SPD_H
#define LOGIT_DDR3_SPD_H

#include <stdint.h>

#define DDR3_SPD_BYTES_NEEDED 146u

struct ddr3_spd_info {
    uint8_t spd_revision;
    uint8_t module_type;
    uint8_t ranks;
    uint8_t device_width;
    uint8_t bus_width;
    uint8_t ecc_bits;
    uint16_t manufacturer_raw;
    uint8_t serial[4];
    uint32_t module_mib;
    char part_number[19];
};

enum ddr3_spd_result {
    DDR3_SPD_OK = 0,
    DDR3_SPD_NOT_DDR3 = -1,
    DDR3_SPD_BAD_CRC = -2,
    DDR3_SPD_BAD_GEOMETRY = -3,
};

/* Decode only fields whose DDR3 SPD layout is fixed. module_mib is capacity
 * represented by this module's chips; it is not firmware-usable RAM. */
int ddr3_spd_decode(const uint8_t spd[DDR3_SPD_BYTES_NEEDED],
                    struct ddr3_spd_info *out);

#endif
