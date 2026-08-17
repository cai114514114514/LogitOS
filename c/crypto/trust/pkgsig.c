#include "pkgsig.h"
#include "crypto.h"

/* See pkgsig.h for the format, the trade the digest-in-header design makes,
 * and -- the part that decides whether any of it means anything -- where the
 * trust root lives and why it is not a file. */

/* Generated from tools/pkgroots/<name>.pub by tools/genpkgroots.py. Defines:
 *     static const struct pkg_root { const char *name; uint8_t key[32]; }
 *         PKG_ROOTS[];
 *     #define PKG_ROOT_N <count>
 * The Makefile gives this object an explicit dependency on the .inc, for the
 * same reason roots.o has one on roots_bundle.inc. */
#include "pkgroots.inc"

static const char LPK_MAGIC[8] = { 'L','O','G','I','T','P','K','G' };
static const char LPK_DOMAIN[] = "LOGITOS-lpk-v1";

static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t rd64(const uint8_t *p)
{ uint64_t v = 0; for (int i=7;i>=0;i--) v = (v<<8) | p[i]; return v; }
static void wr32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i] = (uint8_t)(v >> (8*i)); }
static void wr64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i] = (uint8_t)(v >> (8*i)); }

/* h = SHA-256(DOMAIN || hdr[0..LPK_SIGNED_LEN)). The domain string goes in
 * WITHOUT its NUL and is a fixed literal, so there is no length-extension
 * ambiguity between it and the header. */
static void lpk_sighash(uint8_t h[32], const uint8_t *hdr)
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, LPK_DOMAIN, sizeof LPK_DOMAIN - 1);
    sha256_update(&c, hdr, LPK_SIGNED_LEN);
    sha256_final(&c, h);
}

int pkg_root_count(void) { return PKG_ROOT_N; }
const uint8_t *pkg_root_key(int i)
{ return (i >= 0 && i < PKG_ROOT_N) ? PKG_ROOTS[i].key : 0; }
const char *pkg_root_name(int i)
{ return (i >= 0 && i < PKG_ROOT_N) ? PKG_ROOTS[i].name : 0; }

/* THE BUILT-IN ARRAY IS OUT OF SCOPE FROM HERE DOWN, and by the compiler rather
 * than by convention.
 *
 * pkgsig.h:57-62 promises that moving the trust set off the ISO is "local to
 * pkg_root_count/pkg_root_key". That promise is only worth the line it is
 * written on if every reader of the trust set goes through them -- and until
 * now lpk_verify did not: it looped over PKG_ROOTS directly, four lines below
 * the three accessors it was supposed to use, while /bin/pkgverify --roots (the
 * diagnostic whose entire job is to say who is trusted) did use them. With one
 * compiled-in root the two read the same bytes and nothing diverges. Follow the
 * header, swap the accessors for a disk or a keyring, and the LISTING and the
 * VERDICT come from different sources -- the UI insisting the wrong one is
 * right, which is worse than no UI.
 *
 * `#pragma GCC poison` makes a relapse a compile error at the exact character
 * that causes it, instead of a second source of truth nobody sees until the
 * accessors stop being a wrapper. It costs nothing: PKG_ROOTS and PKG_ROOT_N
 * are referenced nowhere else in the tree (only this file and the generator
 * that emits them). PKG_ROOT_N is #undef'd first because poisoning a live macro
 * name is a diagnostic in its own right. Watched failing by test-pkg-poison,
 * which appends one direct read to a copy of this file and requires cc to
 * refuse it. */
#undef PKG_ROOT_N
#pragma GCC poison PKG_ROOTS PKG_ROOT_N

int lpk_sign(uint8_t *hdr, const char *name,
             const uint8_t *payload, uint64_t payload_len,
             const uint8_t seed[32])
{
    int nl = 0; while (name && name[nl]) nl++;
    if (nl > LPK_NAME_MAX) return -1;

    for (int i = 0; i < LPK_HDR_LEN; i++) hdr[i] = 0;
    for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)LPK_MAGIC[i];
    wr32(hdr + 8, LPK_VERSION);
    wr32(hdr + 12, 0);
    wr64(hdr + 16, payload_len);
    wr32(hdr + 24, (uint32_t)nl);
    wr32(hdr + 28, 0);
    for (int i = 0; i < nl; i++) hdr[32 + i] = (uint8_t)name[i];

    /* payload digest */
    struct sha256 c;
    sha256_init(&c);
    if (payload_len) sha256_update(&c, payload, (size_t)payload_len);
    sha256_final(&c, hdr + 96);

    uint8_t pub[32];
    ed25519_pubkey(pub, seed);
    for (int i = 0; i < 32; i++) hdr[128 + i] = pub[i];

    uint8_t h[32];
    lpk_sighash(h, hdr);
    ed25519_sign(hdr + 160, h, 32, seed, pub);

    crypto_wipe(h, sizeof h);
    return 0;
}

int lpk_verify(const uint8_t *buf, uint64_t len, struct lpk *out, int trust_any)
{
    if (!buf || !out) return LPK_E_FIELD;
    if (len < LPK_HDR_LEN) return LPK_E_SHORT;

    for (int i = 0; i < 8; i++) if (buf[i] != (uint8_t)LPK_MAGIC[i]) return LPK_E_MAGIC;
    if (rd32(buf + 8) != LPK_VERSION) return LPK_E_VERSION;
    /* flags and reserved must be zero. A verifier that ignores an unknown flag
     * is a verifier that can be told to skip a check by a future format it does
     * not understand, which is how "optional signature" bugs happen. */
    if (rd32(buf + 12) != 0 || rd32(buf + 28) != 0) return LPK_E_FIELD;

    uint64_t plen = rd64(buf + 16);
    uint32_t nlen = rd32(buf + 24);
    if (nlen > LPK_NAME_MAX) return LPK_E_FIELD;
    /* The overflow check first, then the length check: plen comes off the wire
     * and LPK_HDR_LEN + plen can wrap on a 64-bit add if it is near 2^64. */
    if (plen > len - LPK_HDR_LEN) return LPK_E_SHORT;

    /* The name must be NUL-padded to the end of its field. Trailing junk after
     * name_len would otherwise be signed-but-invisible: two packages with the
     * same displayed name and different bytes, both with valid signatures. */
    for (uint32_t i = nlen; i < LPK_NAME_MAX; i++) if (buf[32 + i] != 0) return LPK_E_FIELD;
    /* and no embedded NUL or path separator inside it */
    for (uint32_t i = 0; i < nlen; i++) {
        uint8_t ch = buf[32 + i];
        if (ch == 0 || ch == '/' || ch == '\\' || ch < 0x20) return LPK_E_FIELD;
    }

    /* payload digest, before the signature: a cheap check that rejects a
     * corrupted download without doing curve arithmetic on it. */
    const uint8_t *payload = buf + LPK_HDR_LEN;
    uint8_t d[32];
    struct sha256 c;
    sha256_init(&c);
    if (plen) sha256_update(&c, payload, (size_t)plen);
    sha256_final(&c, d);
    {
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= (uint8_t)(d[i] ^ buf[96 + i]);
        if (diff) return LPK_E_DIGEST;
    }

    uint8_t h[32];
    lpk_sighash(h, buf);
#ifdef PKGSIG_BREAK_NO_SIGCHECK
    /* NEGATIVE CONTROL (test-pkg-control). Everything else above still runs --
     * magic, version, flags, name shape, payload digest -- and only the
     * signature is not checked. That is the shape of the real bug: a container
     * validator that never got round to the crypto. */
    (void)h;
#else
    if (!ed25519_verify(buf + 160, h, 32, buf + 128)) return LPK_E_SIG;
#endif

    /* Through the accessors, which are the only readers of the built-in array
     * (see the poison above). `root_index` is an index into pkg_root_count()'s
     * enumeration, so /bin/pkgverify can turn it back into a name with
     * pkg_root_name() and be talking about the same entry.
     *
     * A NULL key is skipped, never matched: pkg_root_key is documented to
     * return NULL for an entry it cannot produce, and a store that answers "I
     * do not have this one" must narrow the trust set, not widen it. `continue`
     * rather than `break` so one unreadable entry cannot hide the ones after
     * it -- silently trusting fewer roots is a refusal a user can see, silently
     * skipping the rest of the table is not. */
    int root = -1;
    const int nroots = pkg_root_count();
    for (int r = 0; r < nroots; r++) {
        const uint8_t *rk = pkg_root_key(r);
        if (!rk) continue;
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= (uint8_t)(rk[i] ^ buf[128 + i]);
        if (!diff) { root = r; break; }
    }
    if (root < 0 && !trust_any) return LPK_E_UNTRUSTED;

    out->payload = payload;
    out->payload_len = plen;
    for (uint32_t i = 0; i < nlen; i++) out->name[i] = (char)buf[32 + i];
    out->name[nlen] = 0;
    for (int i = 0; i < 32; i++) out->signer[i] = buf[128 + i];
    out->root_index = root;
    return LPK_OK;
}
