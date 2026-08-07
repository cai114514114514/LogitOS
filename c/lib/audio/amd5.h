/* c/lib/audio/amd5.h -- MD5 (RFC 1321), used only for FLAC's self-check.
 *
 * This is here rather than in c/crypto because it is not being used as a
 * cryptographic primitive and must not be mistaken for one: MD5 is broken for
 * every security purpose. FLAC's STREAMINFO stores the MD5 of the unencoded
 * audio as an integrity check on lossless reconstruction, so a decoder needs
 * it to verify itself. c/crypto is kernel-side and offers SHA only; audio is a
 * ring-3 library and does not link the kernel's crypto.
 *
 * Verified against the RFC 1321 test suite in tests/unit/flac_test.c.
 */
#ifndef LOGIT_AMD5_H
#define LOGIT_AMD5_H

#include <stdint.h>

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;          /* total bytes fed */
    uint8_t  buf[64];
    unsigned have;         /* bytes buffered */
} amd5;

void amd5_init(amd5 *m);
void amd5_update(amd5 *m, const uint8_t *p, unsigned long n);
void amd5_final(amd5 *m, uint8_t out[16]);

#endif /* LOGIT_AMD5_H */
