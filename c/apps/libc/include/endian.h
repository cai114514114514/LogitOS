#ifndef _ENDIAN_H
#define _ENDIAN_H
/* endian.h for musl libm and ported userland (x86-64 is little-endian). */
#include <stdint.h>
#include <byteswap.h>
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER

#define htobe16(x) bswap_16((uint16_t)(x))
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) bswap_16((uint16_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define htobe32(x) bswap_32((uint32_t)(x))
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) bswap_32((uint32_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define htobe64(x) bswap_64((uint64_t)(x))
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) bswap_64((uint64_t)(x))
#define le64toh(x) ((uint64_t)(x))
#endif
