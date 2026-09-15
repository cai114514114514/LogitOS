/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Conservative JEDEC DDR3 SPD decoding.  The memory controller, firmware
 * reservations and failed training can all make usable RAM smaller than the
 * module sum, so this file never calls module capacity "installed memory". */
#include "ddr3_spd.h"

static uint16_t spd_crc16(const uint8_t *p, unsigned n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1);
    }
    return crc;
}

int ddr3_spd_decode(const uint8_t spd[DDR3_SPD_BYTES_NEEDED],
                    struct ddr3_spd_info *out)
{
    if (!spd || !out || spd[2] != 0x0b) return DDR3_SPD_NOT_DDR3;

    /* DDR3 byte 0 bit 7 changes the CRC coverage from bytes 0..125 to
     * 0..116.  Treating every module as 126-byte coverage silently rejects a
     * valid class of DIMMs; both forms have dedicated fixtures. */
#ifdef X79_SPD_NEG_FIXED_CRC126
    unsigned crc_bytes = 126;
#else
    unsigned crc_bytes = (spd[0] & 0x80u) ? 117u : 126u;
#endif
    uint16_t stored = (uint16_t)spd[126] | ((uint16_t)spd[127] << 8);
    if (spd_crc16(spd, crc_bytes) != stored) return DDR3_SPD_BAD_CRC;

    unsigned density_code = spd[4] & 0x0fu;
    unsigned module_type = spd[3] & 0x0fu;
    unsigned device_code = spd[7] & 0x07u;
    unsigned ranks = ((spd[7] >> 3) & 0x07u) + 1u;
    unsigned bus_code = spd[8] & 0x07u;
    unsigned ext_code = (spd[8] >> 3) & 0x03u;
    if (density_code > 6 || module_type < 1 || module_type > 0x0c ||
        device_code > 3 || bus_code > 3 || ext_code > 1)
        return DDR3_SPD_BAD_GEOMETRY;

    uint64_t density_mbit = 256ull << density_code;
    unsigned device_width = 4u << device_code;
    unsigned bus_width = 8u << bus_code;
    if (bus_width < device_width || bus_width % device_width)
        return DDR3_SPD_BAD_GEOMETRY;
    uint64_t module_mib = (density_mbit / 8u) *
                          (bus_width / device_width) * ranks;
    /* These limits encompass X79-era DDR3 while refusing corrupt geometry
     * that could otherwise turn a diagnostic into an absurd capacity claim. */
    if (module_mib < 128 || module_mib > 131072)
        return DDR3_SPD_BAD_GEOMETRY;

    out->spd_revision = spd[1];
    out->module_type = (uint8_t)module_type;
    out->ranks = (uint8_t)ranks;
    out->device_width = (uint8_t)device_width;
    out->bus_width = (uint8_t)bus_width;
    out->ecc_bits = ext_code ? 8 : 0;
    out->manufacturer_raw = (uint16_t)spd[117] | ((uint16_t)spd[118] << 8);
    for (int i = 0; i < 4; i++) out->serial[i] = spd[122 + i];
    out->module_mib = (uint32_t)module_mib;
    for (int i = 0; i < 18; i++) {
        uint8_t c = spd[128 + i];
        out->part_number[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    int end = 18;
    while (end && out->part_number[end - 1] == ' ') end--;
    out->part_number[end] = 0;
    return DDR3_SPD_OK;
}
