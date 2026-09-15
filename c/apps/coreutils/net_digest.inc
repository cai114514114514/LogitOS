/* SPDX-License-Identifier: MIT */
/* File and HTTP consumers share this adapter, so a checksum cannot silently
 * mean BLAKE2b-256 in one command and truncated BLAKE2b-512 in another.
 * These are unkeyed file fingerprints, not package signer authentication. */
#include "crypto.h"
#include "blake2b.h"
#include "blake3.h"

enum { ND_SHA256, ND_SHA512, ND_BLAKE2B, ND_BLAKE3 };
static const char *const nd_names[] = { "sha256", "sha512", "blake2b", "blake3" };
struct net_digest {
    int alg;
    union { struct sha256 s256; struct sha512 s512;
            struct blake2b b2; struct blake3_hasher b3; } u;
};
static int nd_algorithm(const char *name)
{
    for (int i = 0; i < 4; i++) if (c_streq(name, nd_names[i])) return i;
    return -1;
}
static int nd_size(int alg) { return alg == ND_SHA512 || alg == ND_BLAKE2B ? 64 : 32; }
static void nd_init(struct net_digest *d, int alg)
{
    d->alg = alg;
    switch (alg) {
    case ND_SHA256: sha256_init(&d->u.s256); break;
    case ND_SHA512: sha512_init(&d->u.s512); break;
    case ND_BLAKE2B: blake2b_init(&d->u.b2, 64, NULL, 0); break;
    case ND_BLAKE3: blake3_init(&d->u.b3); break;
    }
}
static void nd_update(struct net_digest *d, const void *p, size_t n)
{
    switch (d->alg) {
    case ND_SHA256: sha256_update(&d->u.s256, p, n); break;
    case ND_SHA512: sha512_update(&d->u.s512, p, n); break;
    case ND_BLAKE2B: blake2b_update(&d->u.b2, p, n); break;
    case ND_BLAKE3: blake3_update(&d->u.b3, p, n); break;
    }
}
static void nd_final(struct net_digest *d, uint8_t out[64])
{
    switch (d->alg) {
    case ND_SHA256: sha256_final(&d->u.s256, out); break;
    case ND_SHA512: sha512_final(&d->u.s512, out); break;
    case ND_BLAKE2B: blake2b_final(&d->u.b2, out); break;
    case ND_BLAKE3: blake3_finalize(&d->u.b3, out); break;
    }
#ifdef NET_DIGEST_NEGCTL
    out[0] ^= 1; /* Must change both local-file and network-body results. */
#endif
}
static int nd_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int nd_expected(int alg, const char *hex, uint8_t out[64])
{
    int n = nd_size(alg);
    if (c_strlen(hex) != n * 2) return -1;
    for (int i = 0; i < n; i++) {
        int a = nd_nibble(hex[2*i]), b = nd_nibble(hex[2*i+1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}
static int nd_equal(const uint8_t *a, const uint8_t *b, int n)
{
    unsigned diff = 0;
    for (int i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}
static void nd_print(int alg, const uint8_t *digest)
{
    static const char hex[] = "0123456789abcdef";
    char text[129]; int n = nd_size(alg);
    for (int i = 0; i < n; i++) {
        text[2*i] = hex[digest[i] >> 4]; text[2*i+1] = hex[digest[i] & 15];
    }
    text[2*n] = 0; outs(text);
}
static int nd_file(int alg, const char *path, uint8_t out[64])
{
    int borrowed = c_streq(path, "-");
    int fd = borrowed ? 0 : sys_open(path, 0);
    if (fd < 0) return -1;
    struct net_digest d; uint8_t buf[4096];
    nd_init(&d, alg);
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) nd_update(&d, buf, (size_t)n);
    int rc = borrowed ? 0 : sys_close(fd);
    /* A short positive read is ordinary; a negative read is not EOF. Never
     * print a digest of a readable prefix after an I/O failure. */
    if (n < 0 || rc < 0) return -1;
    nd_final(&d, out);
    return 0;
}
static int do_checksum(int alg, const char *path, const uint8_t *expected)
{
    uint8_t digest[64];
    if (nd_file(alg, path, digest) < 0) { errs("net: file read failed\n"); return 1; }
    if (expected) {
        if (!nd_equal(digest, expected, nd_size(alg))) { errs("net: checksum mismatch\n"); return 2; }
        outs("verified "); outs(path); outc('\n');
    } else {
        nd_print(alg, digest); outs("  "); outs(path); outc('\n');
    }
    return 0;
}
