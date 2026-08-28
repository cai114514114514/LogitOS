#ifndef LOGIT_CRYPTO_CRC32C_H
#define LOGIT_CRYPTO_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/* CRC-32C (Castagnoli), the checksum used by iSCSI (RFC 3720 Appendix B.4),
 * SCTP (RFC 4960 Appendix B), ext4 metadata and Btrfs -- NOT the CRC-32 this
 * tree already has one of. c/drivers/block/crc32.h's own header calls itself
 * "the one CRC-32 in the tree" and means the IEEE 802.3/zlib/GPT polynomial,
 * reflected 0xEDB88320. This is a DIFFERENT polynomial with a DIFFERENT
 * reflected form, 0x82F63B78, and it is deliberately not named crc32.h/crc32()
 * -- INCDIRS is one flat sorted list (CLAUDE.md's header-collision note) and
 * c/drivers/block/crc32.h already occupies that basename; a second header
 * called crc32.h here would shadow it for the whole kernel build, and a
 * second symbol called crc32() would collide at link. Hence crc32c.h /
 * crc32c() throughout.
 *
 * NO CONSUMER TODAY. This is a primitive shipped for breadth, per the
 * standing instruction: a library function with a known-answer gate makes no
 * claim to anybody, unlike c/crypto/trust (root store) or anything reachable
 * from TLS/aex/login verification, which is exactly where this must NOT be
 * wired without a separate, argued decision. See tests/unit/crc32c_test.c's
 * header for why: growing c/drivers/block/crc32.c to also do Castagnoli
 * would be "widening the one CRC-32 in the tree" into two algorithms sharing
 * one name's worth of trust, which is the shape this tree's "one jar, two
 * doors" rule warns about at the OTHER end -- one name, two meanings.
 *
 * TWO CONVENTIONS EXIST FOR THE OUTPUT WORD AND THIS FILE PICKS ONE.
 * crc32c() below returns the REGISTER value: init 0xFFFFFFFF, the standard
 * reflected byte-at-a-time update, final XOR 0xFFFFFFFF, and NO further byte
 * swap. This is what the SSE4.2 CRC32 instruction produces, what the RevEng
 * CRC catalogue's "check" value is computed as (CRC-32/ISCSI, check=0xe3069283
 * for ASCII "123456789"), and what most software CRC32C users (ext4, Btrfs,
 * SCTP-over-software) treat as "the checksum".
 *
 * RFC 3720 Appendix B.4 prints its five worked examples "as transmitted",
 * which is this register value with its four bytes REVERSED (not a different
 * algorithm -- the wire format iSCSI puts on the network happens to be the
 * little-endian byte order of the register, which reads backwards when
 * printed as a big-endian hex word). 32 zero bytes: register 0x8a9136aa,
 * "as transmitted" (RFC's printed value) aa 36 91 8a. crc32c_test.c computes
 * the register value and byte-swaps it before comparing against the RFC's
 * printed bytes, WITH A COMMENT AT EACH COMPARISON saying so -- a test that
 * silently swapped to make the RFC agree would hide the exact confusion this
 * paragraph exists to pin down. A caller that needs iSCSI/SCTP wire bytes
 * must swap explicitly; this header does not do it for them, because a
 * function that silently returns different byte orders depending on who
 * asks is a worse trap than a caller who has to look this comment up once. */

#define CRC32C_INIT 0xFFFFFFFFu

/* Fold `n` bytes into a running CRC that started at CRC32C_INIT. */
uint32_t crc32c_update(uint32_t crc, const void *data, size_t n);
/* Finish a running CRC (the final inversion). */
static inline uint32_t crc32c_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

/* One-shot register value: crc32c_final(crc32c_update(CRC32C_INIT, data, n)). */
uint32_t crc32c(const void *data, size_t n);

#endif /* LOGIT_CRYPTO_CRC32C_H */
