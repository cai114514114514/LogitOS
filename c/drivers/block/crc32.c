#include "crc32.h"

/* Nibble table (16 entries) rather than the usual 256-entry byte table, the same
 * trade c/net/http/http1.c made: this runs over a 16 KiB GPT entry array a few
 * times per boot, not per pixel, so 64 bytes of table beats 1 KiB of it. Two
 * shifts per byte instead of one. */
static const uint32_t T[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
};

uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ T[crc & 0xF];
        crc = (crc >> 4) ^ T[crc & 0xF];
    }
    return crc;
}

uint32_t crc32(const void *data, size_t n)
{
    return crc32_final(crc32_update(CRC32_INIT, data, n));
}
