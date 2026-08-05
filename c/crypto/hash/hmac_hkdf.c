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
    } else {
        struct sha512 c; sha384_init(&c);
        sha512_update(&c, ipad, B); sha512_update(&c, msg, msglen); sha384_final(&c, inner);
        sha384_init(&c); sha512_update(&c, opad, B); sha512_update(&c, inner, hlen); sha384_final(&c, out);
    }
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
}

/* HKDF-Expand-Label: HkdfLabel = len(out) || "tls13 "+label || context. */
void hkdf_expand_label(int hlen, const uint8_t *secret, const char *label,
                       const uint8_t *ctx, int ctxlen, uint8_t *out, int outlen)
{
    uint8_t info[2 + 1 + 6 + 64 + 1 + 64]; int n = 0;
    info[n++] = (uint8_t)(outlen >> 8); info[n++] = (uint8_t)outlen;
    int ll = 0; while (label[ll]) ll++;
    info[n++] = (uint8_t)(6 + ll);
    const char *p = "tls13 "; for (int i = 0; i < 6; i++) info[n++] = p[i];
    for (int i = 0; i < ll; i++) info[n++] = label[i];
    info[n++] = (uint8_t)ctxlen;
    for (int i = 0; i < ctxlen; i++) info[n++] = ctx[i];
    hkdf_expand(hlen, secret, info, n, out, outlen);
}
