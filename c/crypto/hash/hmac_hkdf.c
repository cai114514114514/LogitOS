#include "crypto.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* Generic hash dispatch by digest length (32=SHA-256, 48=SHA-384). */
static void hash(int hlen, const void *data, size_t len, uint8_t *out)
{
    if (hlen == 32) sha256(data, len, out);
    else            sha384(data, len, out);
}
static int blocklen(int hlen) { return hlen == 32 ? 64 : 128; }

void hmac(int hlen, const uint8_t *key, int keylen,
          const uint8_t *msg, int msglen, uint8_t *out)
{
    if (hlen != 32 && hlen != 48) return;       /* only SHA-256 / SHA-384 are supported */
    int B = blocklen(hlen);
    uint8_t k[128], ipad[128], opad[128], inner[48];
    memset(k, 0, sizeof k);
    if (keylen > B) hash(hlen, key, keylen, k);
    else memcpy(k, key, keylen);
    for (int i = 0; i < B; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    /* inner = H(ipad || msg) */
    if (hlen == 32) {
        struct sha256 c; sha256_init(&c);
        sha256_update(&c, ipad, B); sha256_update(&c, msg, msglen); sha256_final(&c, inner);
        sha256_init(&c); sha256_update(&c, opad, B); sha256_update(&c, inner, hlen); sha256_final(&c, out);
        crypto_wipe(&c, sizeof c);              /* ctx state was key-derived */
    } else {
        struct sha512 c; sha384_init(&c);
        sha512_update(&c, ipad, B); sha512_update(&c, msg, msglen); sha384_final(&c, inner);
        sha384_init(&c); sha512_update(&c, opad, B); sha512_update(&c, inner, hlen); sha384_final(&c, out);
        crypto_wipe(&c, sizeof c);
    }
    /* k/ipad/opad are key-derived; inner is H(ipad||msg). All stay on the
     * stack without a wipe otherwise. */
    crypto_wipe(k, sizeof k); crypto_wipe(ipad, sizeof ipad);
    crypto_wipe(opad, sizeof opad); crypto_wipe(inner, sizeof inner);
}

void hkdf_extract(int hlen, const uint8_t *salt, int saltlen,
                  const uint8_t *ikm, int ikmlen, uint8_t *prk)
{
    if (hlen != 32 && hlen != 48) return;       /* zero[] below is only 48 bytes */
    uint8_t zero[48];
    if (!salt || saltlen == 0) { memset(zero, 0, hlen); salt = zero; saltlen = hlen; }
    hmac(hlen, salt, saltlen, ikm, ikmlen, prk);
}

void hkdf_expand(int hlen, const uint8_t *prk, const uint8_t *info, int infolen,
                 uint8_t *out, int outlen)
{
    /* in[] holds T(prev) + info + ctr; RFC 5869 caps outlen at 255*hlen */
    if (hlen != 32 && hlen != 48) return;
    if (infolen < 0 || infolen > 256 || outlen < 0 || outlen > 255*hlen) return;
    uint8_t t[48]; int tlen = 0, done = 0; uint8_t ctr = 1;
    while (done < outlen) {
        uint8_t in[48 + 256 + 1]; int n = 0;
        for (int i = 0; i < tlen; i++) in[n++] = t[i];      /* T(prev) */
        for (int i = 0; i < infolen; i++) in[n++] = info[i];
        in[n++] = ctr++;
        hmac(hlen, prk, hlen, in, n, t);
        tlen = hlen;
        int take = outlen - done; if (take > hlen) take = hlen;
        memcpy(out + done, t, take); done += take;
    }
    crypto_wipe(t, sizeof t);                   /* T(i) chain is PRK-derived */
}

/* --- TLS 1.2 PRF (RFC 5246 5) ---
 *
 * Nothing about this is HKDF. TLS 1.2 predates HKDF and expands with its own
 * ladder:
 *
 *      A(0) = seed;  A(i) = HMAC(secret, A(i-1))
 *      P_hash(secret, seed) = HMAC(secret, A(1)||seed)
 *                          || HMAC(secret, A(2)||seed) || ...
 *      PRF(secret, label, seed) = P_hash(secret, label || seed)
 *
 * The hash is the one the negotiated cipher suite names (SHA-256 for the
 * *_SHA256 suites, SHA-384 for *_SHA384) -- NOT necessarily SHA-256, which is
 * the single easiest thing to get wrong here: a SHA-256 PRF against an
 * AES_256_GCM_SHA384 server produces keys that decrypt nothing, and the first
 * symptom is a Finished mismatch a whole flight later.
 *
 * (TLS 1.0/1.1's MD5+SHA-1 split PRF is deliberately absent: we do not offer
 * those versions, so implementing their PRF would only create a way to end up
 * using them.) */
void tls12_prf(int hlen, const uint8_t *secret, int seclen, const char *label,
               const uint8_t *seed, int seedlen, uint8_t *out, int outlen)
{
    if (hlen != 32 && hlen != 48) return;
    int ll = 0; while (label[ll]) ll++;
    /* Bounds: the longest label we use is "extended master secret" (22) and the
     * longest seed is server_random||client_random (64); a session hash is 48.
     * Refusing rather than truncating keeps a caller bug from silently
     * producing a key nobody else will derive. */
    if (ll > 32 || seedlen < 0 || seedlen > 128 || outlen < 0) return;

    uint8_t ls[32 + 128]; int lslen = 0;        /* label || seed */
    for (int i = 0; i < ll; i++) ls[lslen++] = (uint8_t)label[i];
    for (int i = 0; i < seedlen; i++) ls[lslen++] = seed[i];

    uint8_t a[48]; int alen = hlen;
    hmac(hlen, secret, seclen, ls, lslen, a);   /* A(1) = HMAC(secret, seed) */

    int done = 0;
    while (done < outlen) {
        uint8_t in[48 + 32 + 128]; int n = 0;
        for (int i = 0; i < alen; i++) in[n++] = a[i];
        for (int i = 0; i < lslen; i++) in[n++] = ls[i];
        uint8_t blk[48];
        hmac(hlen, secret, seclen, in, n, blk);
        int take = outlen - done; if (take > hlen) take = hlen;
        memcpy(out + done, blk, (size_t)take); done += take;
        hmac(hlen, secret, seclen, a, alen, a); /* A(i+1) = HMAC(secret, A(i)) */
        crypto_wipe(in, (size_t)n); crypto_wipe(blk, sizeof blk);
    }
    crypto_wipe(a, sizeof a);                   /* the A() chain is secret-derived */
    crypto_wipe(ls, sizeof ls);
}

/* HKDF-Expand-Label: HkdfLabel = len(out) || "tls13 "+label || context.
 * Returns 0 on success, -1 if label/context/outlen would overflow the
 * fixed info[] buffer (callers in tls.c use short literals + 32-byte hashes,
 * so this never fails in practice -- but a library must not scribble past
 * its stack buffer on a bad caller). */
int hkdf_expand_label(int hlen, const uint8_t *secret, const char *label,
                      const uint8_t *ctx, int ctxlen, uint8_t *out, int outlen)
{
    uint8_t info[2 + 1 + 6 + 64 + 1 + 64]; int n = 0;
    int ll = 0; while (label[ll]) ll++;
    if (hlen != 32 && hlen != 48) return -1;
    /* outlen must also respect hkdf_expand()'s RFC 5869 cap (255*hlen):
     * accepting more would return 0 with the output left unwritten. */
    if (ll > 64 || ctxlen < 0 || ctxlen > 64 || outlen < 0 || outlen > 255*hlen)
        return -1;
    info[n++] = (uint8_t)(outlen >> 8); info[n++] = (uint8_t)outlen;
    info[n++] = (uint8_t)(6 + ll);
    const char *p = "tls13 "; for (int i = 0; i < 6; i++) info[n++] = p[i];
    for (int i = 0; i < ll; i++) info[n++] = label[i];
    info[n++] = (uint8_t)ctxlen;
    for (int i = 0; i < ctxlen; i++) info[n++] = ctx[i];
    hkdf_expand(hlen, secret, info, n, out, outlen);
    crypto_wipe(info, sizeof info);
    return 0;
}
