#include "crypto.h"
#include <stdint.h>
#include <stddef.h>

/* PBKDF2-HMAC-SHA256/512 (RFC 8018 5.2) and the account-record format on top
 * of it.
 *
 * WHY PBKDF2 AND NOT scrypt OR argon2
 * -----------------------------------
 * The honest ranking for a NEW system in 2026 is argon2id > scrypt > PBKDF2.
 * This picks the last of the three on purpose, and the reason is what the three
 * actually cost HERE:
 *
 *   - argon2id and scrypt buy their advantage with MEMORY: the defence is that
 *     an attacker's GPU or ASIC cannot hold 64 MiB per guess. This kernel has
 *     512 MiB total and its memory reclaim path (see CLAUDE.md) is real but
 *     young. A login that allocates 64 MiB is a login that can be turned into
 *     an out-of-memory condition by anyone who can reach the login prompt --
 *     the password hash would become the easiest denial of service on the
 *     machine. A tunable-down argon2 with 8 MiB is argon2 with most of its
 *     advantage traded away, and at that point the comparison is much closer.
 *   - PBKDF2 is HMAC in a loop. HMAC-SHA-256 already exists here, is already
 *     verified against RFC 4231 vectors, and needs 200 bytes of stack. The
 *     whole implementation below is 30 lines, all of which are reviewable.
 *   - The threat model this machine actually has is a stolen disk image, not a
 *     funded offline attack on a password database. There is ONE account.
 *
 * So: PBKDF2-HMAC-SHA256, 600000 iterations by default (OWASP's 2023 figure
 * for this PRF), with the iteration count stored IN the record so it can be
 * raised without invalidating anything. When this machine grows a real
 * multi-user store and a memory subsystem that can absorb it, argon2id is the
 * upgrade, and the record format above is what makes it a migration rather
 * than a flag day: a record beginning "$argon2id$" simply routes elsewhere.
 *
 * WHAT PBKDF2 DOES NOT DEFEND AGAINST, said plainly: it is not memory-hard, so
 * a GPU attacker gets the full parallelism advantage. It stretches; it does not
 * equalise.
 *
 * CONSTANT TIME: the PBKDF2 core is data-independent in its control flow (the
 * loop trip count comes from `iters`, which is public). pwhash_check's digest
 * comparison is a constant-time accumulate-and-compare. The base64 and integer
 * parsing of a stored record are NOT constant time and do not need to be --
 * the record is the thing an attacker already has if they have anything. */

#define PB_MAXH 48

static void hmac_prf(int hlen, const uint8_t *key, int keylen,
                     const uint8_t *msg, int msglen, uint8_t *out)
{ hmac(hlen, key, keylen, msg, msglen, out); }

void pbkdf2(int hlen, const uint8_t *pw, int pwlen,
            const uint8_t *salt, int saltlen, uint32_t iters,
            uint8_t *dk, int dklen)
{
    /* 32 = HMAC-SHA-256, 48 = HMAC-SHA-384 -- exactly what hmac() implements.
     * SHA-512 is NOT offered even though sha512() exists, because hmac() does
     * not take it, and adding a third width to hmac() to serve a KDF nobody
     * asked for is how a verified primitive stops being verified. */
    if ((hlen != 32 && hlen != 48) || iters < 1 || dklen <= 0) return;

    /* The salt||INT(i) buffer. 256 bytes of salt is far past anything sane and
     * bounds what a caller can push onto the kernel stack here. */
    uint8_t block[256 + 4];
    if (saltlen < 0 || saltlen > 256) return;

    uint8_t u[PB_MAXH], t[PB_MAXH];
    int done = 0;
    for (uint32_t i = 1; done < dklen; i++) {
        for (int j = 0; j < saltlen; j++) block[j] = salt[j];
        block[saltlen+0] = (uint8_t)(i >> 24);
        block[saltlen+1] = (uint8_t)(i >> 16);
        block[saltlen+2] = (uint8_t)(i >> 8);
        block[saltlen+3] = (uint8_t)i;

        hmac_prf(hlen, pw, pwlen, block, saltlen + 4, u);
        for (int j = 0; j < hlen; j++) t[j] = u[j];
        for (uint32_t c = 1; c < iters; c++) {
            hmac_prf(hlen, pw, pwlen, u, hlen, u);
            for (int j = 0; j < hlen; j++) t[j] ^= u[j];
        }
        int n = dklen - done; if (n > hlen) n = hlen;
        for (int j = 0; j < n; j++) dk[done + j] = t[j];
        done += n;
    }
    crypto_wipe(u, sizeof u);
    crypto_wipe(t, sizeof t);
    crypto_wipe(block, sizeof block);
}

/* ------------------------------------------------------- the record format -- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_enc(char *out, int max, const uint8_t *in, int len)
{
    int n = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = len - i;
        if (rem > 1) v |= (uint32_t)in[i+1] << 8;
        if (rem > 2) v |= in[i+2];
        int chars = rem > 2 ? 4 : rem + 1;
        if (n + 4 > max) return -1;
        for (int k = 0; k < 4; k++)
            out[n+k] = (k < chars) ? B64[(v >> (18 - 6*k)) & 63] : '=';
        n += 4;
    }
    if (n >= max) return -1;
    out[n] = 0;
    return n;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_dec(uint8_t *out, int max, const char *in, int len)
{
    int n = 0; uint32_t acc = 0; int bits = 0;
    for (int i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int v = b64_val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static int str_eqn(const char *a, const char *b, int n)
{ for (int i=0;i<n;i++) if (a[i] != b[i]) return 0; return 1; }

#define PWHASH_SALT 16
#define PWHASH_DK   32

int pwhash_make(char *out, int max, const char *password,
                uint32_t iters, void (*randbytes)(uint8_t *, int))
{
    if (!out || !password || !randbytes) return -1;
    if (iters < 1) iters = 600000;

    uint8_t salt[PWHASH_SALT], dk[PWHASH_DK];
    randbytes(salt, PWHASH_SALT);

    int pwlen = 0; while (password[pwlen]) pwlen++;
    pbkdf2(32, (const uint8_t *)password, pwlen, salt, PWHASH_SALT, iters, dk, PWHASH_DK);

    const char *pfx = "$pbkdf2-sha256$";
    int n = 0;
    for (int i = 0; pfx[i]; i++) { if (n >= max) return -1; out[n++] = pfx[i]; }
    /* iteration count, decimal */
    char num[12]; int nl = 0;
    uint32_t v = iters;
    do { num[nl++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (nl--) { if (n >= max) return -1; out[n++] = num[nl]; }
    if (n >= max) return -1; out[n++] = '$';

    int e = b64_enc(out + n, max - n, salt, PWHASH_SALT);
    if (e < 0) return -1; n += e;
    if (n >= max) return -1; out[n++] = '$';
    e = b64_enc(out + n, max - n, dk, PWHASH_DK);
    if (e < 0) return -1; n += e;

    crypto_wipe(dk, sizeof dk);
    return n;
}

int pwhash_check(const char *record, const char *password)
{
    if (!record || !password) return 0;
    const char *pfx = "$pbkdf2-sha256$";
    int pl = 0; while (pfx[pl]) pl++;
    int rl = 0; while (record[rl]) rl++;
    if (rl < pl || !str_eqn(record, pfx, pl)) return 0;

    const char *p = record + pl;
    uint32_t iters = 0; int digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (iters > 100000000u) return 0;              /* refuse an absurd cost */
        iters = iters * 10 + (uint32_t)(*p - '0'); p++; digits++;
    }
    if (!digits || *p != '$' || iters < 1) return 0;
    p++;

    const char *salt_b = p;
    while (*p && *p != '$') p++;
    if (*p != '$') return 0;
    int salt_l = (int)(p - salt_b); p++;
    const char *dk_b = p;
    int dk_l = 0; while (dk_b[dk_l]) dk_l++;

    uint8_t salt[64], want[PB_MAXH], got[PB_MAXH];
    int sn = b64_dec(salt, sizeof salt, salt_b, salt_l);
    int wn = b64_dec(want, sizeof want, dk_b, dk_l);
    if (sn <= 0 || wn <= 0 || wn > PB_MAXH) return 0;

    int pwlen = 0; while (password[pwlen]) pwlen++;
    pbkdf2(32, (const uint8_t *)password, pwlen, salt, sn, iters, got, wn);

    uint8_t diff = 0;
    for (int i = 0; i < wn; i++) diff |= (uint8_t)(got[i] ^ want[i]);
    crypto_wipe(got, sizeof got);
    crypto_wipe(want, sizeof want);
    return diff == 0;
}
