/* Command-line driver for run-aes-gcm-siv-openssl.sh: the OPENSSL side of the
 * differential. Links libcrypto (OpenSSL 3.5+, which has AES-*-GCM-SIV as a
 * named EVP cipher) and exposes the exact same hex-argv contract as
 * aes_gcm_siv_cli.c so the shell script can treat both sides identically.
 * Host-only glue; not part of c/crypto and never linked into the kernel.
 *
 *   seal <keyhex> <noncehex> <aadhex> <pthex>          -> <ct||tag hex>
 *   open <keyhex> <noncehex> <aadhex> <cthex> <taghex> -> <pt hex>, or "FAIL"+exit 1
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <openssl/evp.h>
#include <openssl/err.h>

static int hex2bin(const char *hex, unsigned char *out, int maxout)
{
    int n = 0;
    size_t len = strlen(hex);
    if (len % 2) return -1;
    for (size_t i = 0; i < len; i += 2) {
        unsigned v;
        if (sscanf(hex + i, "%2x", &v) != 1) return -1;
        if (n >= maxout) return -1;
        out[n++] = (unsigned char)v;
    }
    return n;
}

static void bin2hex(const unsigned char *b, int n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2*i] = H[b[i]>>4]; out[2*i+1] = H[b[i]&15]; }
    out[2*n] = 0;
}

/* OpenSSL 3.x ships AES-*-GCM-SIV only as a provider-fetched cipher (no
 * EVP_aes_128_gcm_siv() legacy constructor exists), hence EVP_CIPHER_fetch
 * rather than the older EVP_aes_128_gcm()-style call the GCM code uses. */
static EVP_CIPHER *pick_cipher(int keylen)
{
    return EVP_CIPHER_fetch(NULL, keylen == 16 ? "AES-128-GCM-SIV" : "AES-256-GCM-SIV", NULL);
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: seal|open ...\n"); return 2; }
    unsigned char key[32], nonce[12], aad[256], data[256], tag[16];
    int keylen = hex2bin(argv[2], key, sizeof key);
    int noncelen = hex2bin(argv[3], nonce, sizeof nonce);
    if (noncelen != 12 || (keylen != 16 && keylen != 32)) {
        fprintf(stderr, "bad key/nonce length\n"); return 2;
    }
    EVP_CIPHER *cipher = pick_cipher(keylen);
    if (!cipher) { fprintf(stderr, "this openssl has no AES-%d-GCM-SIV\n", keylen*8); return 3; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { fprintf(stderr, "EVP_CIPHER_CTX_new failed\n"); return 2; }

    if (strcmp(argv[1], "seal") == 0) {
        if (argc != 6) { fprintf(stderr, "seal needs aadhex pthex\n"); return 2; }
        int aadlen = hex2bin(argv[4], aad, sizeof aad);
        int ptlen = hex2bin(argv[5], data, sizeof data);
        unsigned char ct[256]; int outl = 0, tmp = 0;
        if (EVP_EncryptInit_ex(ctx, cipher, NULL, key, nonce) != 1) goto sslfail;
        if (aadlen > 0 && EVP_EncryptUpdate(ctx, NULL, &tmp, aad, aadlen) != 1) goto sslfail;
        if (EVP_EncryptUpdate(ctx, ct, &outl, data, ptlen) != 1) goto sslfail;
        if (EVP_EncryptFinal_ex(ctx, ct + outl, &tmp) != 1) goto sslfail;
        outl += tmp;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) != 1) goto sslfail;
        char hex[520];
        bin2hex(ct, ptlen, hex);
        bin2hex(tag, 16, hex + ptlen*2);
        printf("%s\n", hex);
        EVP_CIPHER_CTX_free(ctx);
        return 0;
    }
    if (strcmp(argv[1], "open") == 0) {
        if (argc != 7) { fprintf(stderr, "open needs aadhex cthex taghex\n"); return 2; }
        int aadlen = hex2bin(argv[4], aad, sizeof aad);
        int ctlen = hex2bin(argv[5], data, sizeof data);
        int taglen = hex2bin(argv[6], tag, sizeof tag);
        if (taglen != 16) { fprintf(stderr, "bad tag length\n"); return 2; }
        unsigned char pt[256]; int outl = 0, tmp = 0;
        /* Unlike AES-GCM, SIV's CTR keystream is derived FROM the tag (the
         * tag doubles as the initial counter block, RFC 8452 section 5), so
         * OpenSSL's provider needs SET_TAG before it can produce any
         * plaintext at all -- SET_TAG after DecryptUpdate (the order that
         * works for plain GCM) fails authentication here even on a genuine
         * ciphertext, because Update has nothing to decrypt with yet. */
        if (EVP_DecryptInit_ex(ctx, cipher, NULL, key, nonce) != 1) goto sslfail;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, tag) != 1) goto sslfail;
        if (aadlen > 0 && EVP_DecryptUpdate(ctx, NULL, &tmp, aad, aadlen) != 1) goto sslfail;
        if (EVP_DecryptUpdate(ctx, pt, &outl, data, ctlen) != 1) goto sslfail;
        if (EVP_DecryptFinal_ex(ctx, pt + outl, &tmp) != 1) {
            printf("FAIL\n");
            EVP_CIPHER_CTX_free(ctx);
            return 1;
        }
        outl += tmp;
        char hex[520];
        bin2hex(pt, outl, hex);
        printf("%s\n", hex);
        EVP_CIPHER_CTX_free(ctx);
        return 0;
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    EVP_CIPHER_CTX_free(ctx);
    return 2;

sslfail:
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return 3;
}
