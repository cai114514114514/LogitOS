/* See aexsig.h for the scheme, the domain-separation argument, and what a
 * verdict does and does not mean. This file has no state of its own -- the
 * trust roots it walks in aex_sig_verify() are pkgsig.c's, read only through
 * the accessors pkgsig.h exports, the same ones lpk_verify uses. */
#include "aexsig.h"
#include "pkgsig.h"   /* pkg_root_count/pkg_root_key: the SAME trust roots .lpk uses */
#include "crypto.h"

static void wr64(uint8_t *p, uint64_t v)
{ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

void aex_sig_hash(uint8_t h[32], uint64_t elf_size, const uint8_t elf_sha256[32])
{
    uint8_t manifest[8 + 32];
    wr64(manifest, elf_size);
    for (int i = 0; i < 32; i++) manifest[8 + i] = elf_sha256[i];

    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, AEX_SIG_DOMAIN, sizeof(AEX_SIG_DOMAIN) - 1);
    sha256_update(&c, manifest, sizeof manifest);
    sha256_final(&c, h);
}

void aex_sig_sign(uint8_t out[AEX_SIG_LEN], uint64_t elf_size,
                   const uint8_t elf_sha256[32], const uint8_t seed[32])
{
    uint8_t pub[32];
    ed25519_pubkey(pub, seed);
    for (int i = 0; i < 32; i++) out[i] = pub[i];

    uint8_t h[32];
    aex_sig_hash(h, elf_size, elf_sha256);
    ed25519_sign(out + 32, h, 32, seed, pub);
    crypto_wipe(h, sizeof h);
}

int aex_sig_verify(const uint8_t sigrec[AEX_SIG_LEN], uint64_t elf_size,
                    const uint8_t elf_sha256[32], int *root_index_out)
{
    if (root_index_out) *root_index_out = -1;

    const uint8_t *pub = sigrec;
    const uint8_t *sig = sigrec + 32;

    uint8_t h[32];
    aex_sig_hash(h, elf_size, elf_sha256);
    /* Every input here is public (the image, its digest, the claimed signer
     * key) -- ed25519_verify makes no constant-time claim and needs none;
     * see crypto.h's note on ed25519_verify. */
    if (!ed25519_verify(sig, h, 32, pub))
        return AEX_SIG_INVALID;

    const int nroots = pkg_root_count();
    for (int r = 0; r < nroots; r++) {
        const uint8_t *rk = pkg_root_key(r);
        if (!rk) continue;                  /* a store that cannot produce an
                                              * entry narrows trust, never widens it */
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= (uint8_t)(rk[i] ^ pub[i]);
        if (!diff) {
            if (root_index_out) *root_index_out = r;
            return AEX_SIG_OK;
        }
    }
    return AEX_SIG_UNTRUSTED;
}
