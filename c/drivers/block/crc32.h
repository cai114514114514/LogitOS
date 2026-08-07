#ifndef LOGIT_BLK_CRC32_H
#define LOGIT_BLK_CRC32_H

#include <stdint.h>
#include <stddef.h>

/* CRC-32 (the IEEE 802.3 / zlib / GPT polynomial, reflected 0xEDB88320).
 *
 * GPT needs this twice per disk -- once over the header with its own CRC field
 * zeroed, once over the whole partition-entry array -- and the entry array is
 * read a sector at a time, so the streaming form is the one that matters here:
 * the array can be checksummed without ever holding all of it in memory.
 *
 * There were already two copies of this polynomial in the tree when this was
 * written, but neither is linkable from the kernel: c/net/http/http1.c's is a
 * file-static in a ring-3-only translation unit (RING3_NET is filtered out of
 * C_SRC), and c/apps/video/vidcheck.c's is inside a userland program. This is
 * the kernel's copy, and it is the one a future consolidation should keep --
 * it is the only one with a streaming API and a test. */

#define CRC32_INIT 0xFFFFFFFFu

/* Fold `n` bytes into a running CRC that started at CRC32_INIT. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);
/* Finish a running CRC (the final inversion). */
static inline uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

/* One-shot: crc32_final(crc32_update(CRC32_INIT, data, n)). */
uint32_t crc32(const void *data, size_t n);

#endif /* LOGIT_BLK_CRC32_H */
