#include <stdint.h>
#include <stddef.h>
#include "tls.h"
#include "tls_int.h"
#include "ocsp.h"
#include "tcp.h"
#include "net.h"
#include "pit.h"
#include "crypto.h"
#include "x509.h"
#include "roots.h"
#include "kprintf.h"
#include "rng.h"
#include "mlkem.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

static struct tls_sess sessions[TLS_MAX_SESSIONS];

/* Interned once for the whole-handshake span; see tls_prof.h for why this one
 * cannot use the lexical KPROF_BEGIN/END pair. */
static int tls_hs_slot = -1;

static struct tls_sess *sess_of(int id)
{
    if (id < 0 || id >= TLS_MAX_SESSIONS || !sessions[id].used) return 0;
    return &sessions[id];
}

/* Client nonce / ephemeral keys come from the kernel CSPRNG (kernel/rng.c:
 * SHA-256 DRBG seeded from RDSEED/RDRAND + RDTSC), not a TLS-local PRNG. */
static void rand_bytes(uint8_t *b, int n) { kernel_random_bytes(b, n); }
static uint32_t rand_u32(void) { uint32_t v; rand_bytes((uint8_t *)&v, 4); return v; }

/* Terminal failure. Every secret is wiped immediately -- the slot itself stays
 * allocated so the caller can read the reason back before tls_close(). */
int tls_fail(struct tls_sess *s, int rc)
{
    tlsprof_close(&s->prof);                 /* a failed handshake still cost time */
    crypto_wipe(&s->sec, sizeof s->sec);
    crypto_wipe(&s->cr, sizeof s->cr);
    crypto_wipe(&s->cw, sizeof s->cw);
    crypto_wipe(s->priv, sizeof s->priv);
    crypto_wipe(s->master, sizeof s->master);
    s->state = TS_FAILED;
    s->err = rc;
    return rc;
}
#define fail(s, rc) tls_fail((s), (rc))

/* ------------------------------------------------------------------ record
 * I/O. Both halves are strictly non-blocking: they move whatever TCP will give
 * or take right now and report "would block" otherwise. Nothing in this file
 * calls net_poll() -- that is the caller's job, which is what makes the whole
 * handshake steppable. */

/* Push queued bytes at TCP. 1 = everything drained, 0 = would block, -1 fatal. */
int tls_tx_flush(struct tls_sess *s)
{
    while (s->txoff < s->txlen) {
        int n = tcp_send(s->tcp, s->tx + s->txoff, s->txlen - s->txoff);
        if (n < 0) return -1;
        if (n == 0) return 0;
        s->txoff += n;
    }
    s->txlen = s->txoff = 0;
    return 1;
}

/* Frame body[len] as a record and queue it. -1 if it would not fit (a caller
 * bug: every producer here bounds its own output). */
int tls_tx_queue(struct tls_sess *s, uint8_t type, const uint8_t *body, int len)
{
    if (s->txoff) {                          /* compact away what TCP already took */
        memmove(s->tx, s->tx + s->txoff, (size_t)(s->txlen - s->txoff));
        s->txlen -= s->txoff; s->txoff = 0;
    }
    if (len < 0 || s->txlen + 5 + len > (int)sizeof s->tx) return -1;
    uint8_t *p = s->tx + s->txlen;
    p[0] = type; p[1] = 0x03; p[2] = 0x03;
    p[3] = (uint8_t)(len >> 8); p[4] = (uint8_t)len;
    memcpy(p + 5, body, (size_t)len);
    s->txlen += 5 + len;
    return 0;
}

/* Try to expose one complete record. 1 = s->rectype/s->reclen describe a record
 * whose body starts at s->rxrec+5, 0 = need more bytes, -1 = fatal. */
int tls_rec_pull(struct tls_sess *s)
{
    for (;;) {
        if (s->rxlen >= 5) {
            int blen = (s->rxrec[3] << 8) | s->rxrec[4];
            if (blen > REC_BODY_MAX) return -1;      /* over RFC 8446 5.2's cap */
            if (s->rxlen >= 5 + blen) {
                s->rectype = s->rxrec[0];
                s->reclen = blen;
                return 1;
            }
        }
        int room = (int)sizeof s->rxrec - s->rxlen;
        if (room <= 0) return -1;
        int n = tcp_recv(s->tcp, s->rxrec + s->rxlen, room);
        if (n > 0) { s->rxlen += n; continue; }
        if (n < 0) return -1;                        /* closed and drained */
        return 0;
    }
}

void tls_rec_drop(struct tls_sess *s)
{
    int used = 5 + s->reclen;
    if (used > s->rxlen) used = s->rxlen;
    memmove(s->rxrec, s->rxrec + used, (size_t)(s->rxlen - used));
    s->rxlen -= used;
    s->reclen = 0;
}

/* Compatibility shims so the TLS 1.3 body below reads as it did before the
 * split; the exported names are what tls12.c uses. */
#define tx_flush(s)               tls_tx_flush(s)
#define tx_queue(s, t, b, l)      tls_tx_queue((s), (t), (b), (l))
#define rec_pull(s)               tls_rec_pull(s)
#define rec_drop(s)               tls_rec_drop(s)

/* --- AEAD record seal/open (TLS 1.3) --- */
static void make_nonce(const struct aead *a, uint8_t nonce[12])
{
    memcpy(nonce, a->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(a->seq >> (8 * i));
}

/* Dispatch to whichever AEAD the suite named. Shared with tls12.c: the record
 * framing differs completely between the two versions, the primitive call does
 * not. */
void tls_aead_encrypt(const struct aead *a, const uint8_t nonce[12],
                      const uint8_t *aad, int aadlen,
                      const uint8_t *pt, int len, uint8_t *ct, uint8_t *tag)
{
    /* The span is on the dispatcher rather than on each call site because this
     * is the only place ALL bulk record crypto passes through, for both protocol
     * versions and both directions -- which makes tls_aead_seal/open the pair of
     * numbers the "is TCG-emulated AES-NI actually faster than the C path"
     * question is asked of. */
    TLSPROF_BEGIN(tls_aead_seal);
    if (a->alg == AEAD_CHACHA20)         chacha20_poly1305_seal(a->key, nonce, aad, aadlen, pt, len, ct, tag);
    else if (a->alg == AEAD_AES_256_GCM) aes256_gcm_seal(a->key, nonce, aad, aadlen, pt, len, ct, tag);
    else                                 aes128_gcm_seal(a->key, nonce, aad, aadlen, pt, len, ct, tag);
    TLSPROF_END(tls_aead_seal);
}

int tls_aead_decrypt(const struct aead *a, const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *ct, int len, const uint8_t *tag, uint8_t *pt)
{
    TLSPROF_BEGIN(tls_aead_open);
    int rc;
    if (a->alg == AEAD_CHACHA20)         rc = chacha20_poly1305_open(a->key, nonce, aad, aadlen, ct, len, tag, pt);
    else if (a->alg == AEAD_AES_256_GCM) rc = aes256_gcm_open(a->key, nonce, aad, aadlen, ct, len, tag, pt);
    else                                 rc = aes128_gcm_open(a->key, nonce, aad, aadlen, ct, len, tag, pt);
    TLSPROF_END(tls_aead_open);
    return rc;
}

/* Encrypt (content || inner_type) into a record body; returns the body length.
 * The staging buffer is static rather than automatic to keep it off the kernel
 * stack; that is safe with several live sessions because a seal never yields --
 * it runs start to finish inside one call. */
static int aead_seal(struct aead *a, uint8_t inner_type, const uint8_t *content, int clen,
                     uint8_t *out)
{
    static uint8_t plain[SEND_REC_MAX + 1];
    if (clen < 0 || clen > SEND_REC_MAX) return -1;
    memcpy(plain, content, (size_t)clen); plain[clen] = inner_type;
    int plen = clen + 1;
    int rlen = plen + 16;
    uint8_t aad[5] = { REC_APPDATA, 0x03, 0x03, (uint8_t)(rlen >> 8), (uint8_t)rlen };
    uint8_t nonce[12]; make_nonce(a, nonce);
    tls_aead_encrypt(a, nonce, aad, 5, plain, plen, out, out + plen);
    a->seq++;
    crypto_wipe(plain, (size_t)plen);
    return rlen;
}

/* Decrypt a record body; returns plaintext len and *inner_type, or -1. `out`
 * must have room for blen-16 bytes. */
static int aead_open(struct aead *a, const uint8_t *body, int blen,
                     uint8_t *out, uint8_t *inner_type)
{
    if (blen < 17) return -1;
    int plen = blen - 16;
    uint8_t aad[5] = { REC_APPDATA, 0x03, 0x03, (uint8_t)(blen >> 8), (uint8_t)blen };
    uint8_t nonce[12]; make_nonce(a, nonce);
    if (tls_aead_decrypt(a, nonce, aad, 5, body, plen, body + plen, out)) return -1;
    a->seq++;
    while (plen > 0 && out[plen - 1] == 0) plen--;       /* strip padding */
    if (plen == 0) return -1;
    *inner_type = out[--plen];                            /* trailing real type */
    return plen;
}

/* --- key schedule helpers --- */
/* Labels are all <=12-char literals and ctx is a 32-byte hash (or NULL), so
 * hkdf_expand_label()'s bounds are always satisfied and it cannot fail here. */
static void derive_secret(int hl, const uint8_t *secret, const char *label,
                          const uint8_t *thash, uint8_t *out)
{ hkdf_expand_label(hl, secret, label, thash, hl, out, hl); }

/* The empty-transcript hash the key schedule uses as `context` for the two
 * "derived" steps. Hash-dependent, and a SHA-256 one under a SHA-384 suite is
 * both the wrong value and the wrong length. */
static void empty_hash(int hl, uint8_t *out)
{ if (hl == 48) sha384("", 0, out); else sha256("", 0, out); }

static void traffic_keys(const uint8_t *secret, int suite, struct aead *a)
{
    /* Three independent things the suite decides, and they do not move
     * together: 0x1302 is a 32-byte key like ChaCha20 but a SHA-384 schedule
     * unlike it, and a 12-byte IV like both. Deriving each from the suite
     * rather than from one another is what keeps that from becoming a table
     * with a wrong row. */
    int hl = tls13_suite_hash(suite);
    a->alg = (suite == TLS_CHACHA20_POLY1305_SHA256) ? AEAD_CHACHA20
           : (suite == TLS_AES_256_GCM_SHA384)       ? AEAD_AES_256_GCM
                                                     : AEAD_AES_128_GCM;
    a->keylen = (a->alg == AEAD_AES_128_GCM) ? 16 : 32;
    a->ivlen = 12;
    a->explicit_nonce = 0;                   /* TLS 1.3 has no explicit nonce */
    hkdf_expand_label(hl, secret, "key", 0, 0, a->key, a->keylen);
    hkdf_expand_label(hl, secret, "iv", 0, 0, a->iv, 12);
    a->seq = 0;
}

static void transcript_hash(const struct sha256 *running, uint8_t out[32])
{ struct sha256 c = *running; sha256_final(&c, out); }

/* --- the two-hash transcript ---
 * Every handshake byte goes into both a SHA-256 and a SHA-384 running hash,
 * because the hash the handshake is *actually* keyed on is chosen by a cipher
 * suite the server does not name until the ServerHello -- and TLS 1.2's
 * AES_256_GCM_SHA384 suites, which 1.2 servers routinely prefer, key on
 * SHA-384. tls_th_hash then hands back whichever s->hashlen says (32 until the
 * suite is known, which is what the TLS 1.3 path uses throughout). */
void tls_th_update(struct tls_sess *s, const void *p, int n)
{
    if (n <= 0) return;
    sha256_update(&s->th, p, (size_t)n);
    sha512_update(&s->th384, p, (size_t)n);
}

void tls_th_hash(const struct tls_sess *s, uint8_t *out)
{
    if (s->hashlen == 48) { struct sha512 c = s->th384; sha384_final(&c, out); }
    else                  { struct sha256 c = s->th;    sha256_final(&c, out); }
}

/* big-endian helpers for building messages */
static int put_u16(uint8_t *p, int v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; return 2; }

/* ------------------------------------------------------------------- groups */

/* Shims, as above: the TLS 1.3 body below keeps the short local names. */
#define group_name(g)         tls_group_name(g)
#define group_supported(g)    tls_group_supported(g)
#define gen_share(s)          tls_gen_share(s)
#define compute_shared(s,p,l,o,ol)  tls_compute_shared((s),(p),(l),(o),(ol))
#define log_alert(b,l)        tls_log_alert((b),(l))

static int group_curve(int grp) { return grp == GRP_P256 ? 256 : grp == GRP_P384 ? 384 : 0; }
const char *tls_group_name(int grp)
{
    return grp == GRP_X25519 ? "x25519" : grp == GRP_P256 ? "secp256r1"
         : grp == GRP_P384 ? "secp384r1"
         : grp == GRP_X25519MLKEM768 ? "X25519MLKEM768" : "?";
}

/* "Is this an ECDHE curve we can do a ServerKeyExchange over?" -- which is what
 * TLS 1.2 is asking when it calls this (tls12.c:465). The hybrid is
 * deliberately NOT reported here even though we support it: it is a KEM and
 * TLS 1.2 has no message that carries an encapsulation. Reporting it would let
 * a 1.2 server name 0x11ec as its curve and walk us into the hybrid path with
 * nothing to decapsulate. Refusing it structurally beats adding a version check
 * to the 1.2 reader, which is a check somebody can later delete without seeing
 * why it was there. */
int tls_group_supported(int grp)
{ return grp == GRP_X25519 || grp == GRP_P256 || grp == GRP_P384; }

/* The TLS 1.3 key_share list, which is the above PLUS the hybrid. Used for the
 * HelloRetryRequest retry group, where a server may legitimately ask us to
 * move to any group we offered. */
int tls_group_supported13(int grp)
{ return tls_group_supported(grp) || grp == GRP_X25519MLKEM768; }

/* Generate a fresh ephemeral private key + public share for s->group.
 * The EC path retries on an out-of-range scalar rather than reducing one: a
 * reduction would bias the private key toward small values. Eight tries makes
 * the failure probability (about 2^-32 per try for P-256) unreachable. */
int tls_gen_share(struct tls_sess *s)
{
    TLSPROF_BEGIN(tls_kx_keygen);
    int rc = -1;
    s->group2 = 0;
    if (s->group == GRP_X25519MLKEM768) {
        /* Hybrid: an ML-KEM keypair and an X25519 keypair, independent of one
         * another. priv = x25519 scalar || ML-KEM dk; pub = ek || x25519 pub
         * (see tls_int.h for how that wire order was established).
         *
         * group2 makes the SAME x25519 key pair a second, bare offer in the
         * ClientHello, so a server with no PQ support answers immediately
         * instead of spending a HelloRetryRequest on us. */
        rand_bytes(s->priv, HYB_X_LEN);
        x25519_base(s->pub + HYB_EK_LEN, s->priv);
        mlkem768_keygen(s->pub, s->priv + HYB_DK_OFF);   /* ek -> pub, dk -> priv */
        s->publen = HYB_SHARE_CLI;
        s->group2 = GRP_X25519;
        rc = 0;
    } else if (s->group == GRP_X25519) {
        rand_bytes(s->priv, 32);
        x25519_base(s->pub, s->priv);
        s->publen = 32;
        rc = 0;
    } else {
        int curve = group_curve(s->group);
        int flen = curve / 8;
        for (int try = 0; curve && try < 8; try++) {
            rand_bytes(s->priv, flen);
            if (ecdh_keygen(curve, s->priv, rand_u32(), s->pub) == 0) {
                s->publen = 1 + 2 * flen;
                rc = 0;
                break;
            }
        }
    }
    TLSPROF_END(tls_kx_keygen);
    return rc;
}

/* Derive the ECDHE shared secret from the server's key share. */
int tls_compute_shared(struct tls_sess *s, const uint8_t *spub, int splen,
                       uint8_t *out, int *outlen)
{
    TLSPROF_BEGIN(tls_kx_shared);
    int rc = -1;
    if (s->group == GRP_X25519MLKEM768) {
        /* The server's share is ct || x25519_pub. Decapsulate, then do the
         * X25519, and concatenate the two secrets ML-KEM FIRST -- the same
         * order as the key shares.
         *
         * THERE IS NO ERROR PATH OUT OF THE DECAPSULATION AND THERE MUST NOT
         * BE. ML-KEM rejects implicitly: a ct that does not re-encrypt to
         * itself yields a pseudorandom secret rather than a failure, so a
         * wrong ct simply produces a key the server does not share and the
         * handshake dies at the server Finished MAC. Turning that into an
         * early return here would hand an on-path attacker a decryption
         * oracle -- they could tell a mauled ciphertext that decrypted from
         * one that did not, which is exactly what the FO transform exists to
         * deny. See mlkem.h.
         *
         * The X25519 half keeps its contributory check, and that is the half
         * where a zero result IS a refusal: an all-zero X25519 output means
         * the peer sent a low-order point. The asymmetry is deliberate -- the
         * two halves have different failure semantics and combining them into
         * one "did the key exchange work" flag would flatten that. Security
         * holds if EITHER half is sound, which is the whole point of hybrid. */
        if (splen == HYB_SHARE_SRV) {
            mlkem768_decaps(s->priv + HYB_DK_OFF, spub, out);
            uint8_t xs[32];
            x25519(xs, s->priv, spub + HYB_CT_LEN);
            uint8_t z = 0; for (int i = 0; i < 32; i++) z |= xs[i];
            if (z) {
                for (int i = 0; i < 32; i++) out[32 + i] = xs[i];
                *outlen = HYB_SS_LEN;
                rc = 0;
            }
            crypto_wipe(xs, sizeof xs);
        }
    } else if (s->group == GRP_X25519) {
        if (splen == 32) {
            x25519(out, s->priv, spub);
            /* RFC 7748/8446 contributory check: an all-zero shared secret means
             * the peer sent a low-order point, so the "secret" is one the peer
             * chose. */
            uint8_t z = 0; for (int i = 0; i < 32; i++) z |= out[i];
            if (z) { *outlen = 32; rc = 0; }
        }
    } else {
        int curve = group_curve(s->group);
        /* ecdh_shared range- and on-curve-checks the peer point; with cofactor 1
         * that rules out invalid-curve and small-subgroup attacks outright. */
        if (curve && ecdh_shared(curve, s->priv, rand_u32(), spub, splen, out) == 0) {
            *outlen = curve / 8;
            rc = 0;
        }
    }
    TLSPROF_END(tls_kx_shared);
    return rc;
}

/* --------------------------------------------------------------- ClientHello */

/* Append the ALPN extension for the comma-separated list in s->alpn_offer.
 * Returns bytes written (0 if no list was configured), -1 if it would overflow. */
static int put_alpn(struct tls_sess *s, uint8_t *p, int room)
{
    if (!s->alpn_offer[0]) return 0;
    if (room < 6 + (int)sizeof s->alpn_offer) return -1;
    int n = 0;
    n += put_u16(p + n, EXT_ALPN);
    int extlen_at = n; n += 2;
    int listlen_at = n; n += 2;
    const char *q = s->alpn_offer;
    while (*q) {
        const char *start = q;
        while (*q && *q != ',') q++;
        int l = (int)(q - start);
        if (l > 0 && l < 256) { p[n++] = (uint8_t)l; memcpy(p + n, start, (size_t)l); n += l; }
        if (*q == ',') q++;
    }
    put_u16(p + listlen_at, n - listlen_at - 2);
    put_u16(p + extlen_at, n - extlen_at - 2);
    return n;
}

/* Build the ClientHello. Called for CH1 and again for CH2 after a
 * HelloRetryRequest: everything except the key_share (and the added cookie) is
 * byte-identical, which RFC 8446 4.1.2 requires -- the server hashes both. */
/* Build a ClientHello. On return *binder_off is 0 for a full handshake, or the
 * offset of the PSK binder that step_send_ch must still fill in, with
 * *trunc_len giving the length of the prefix the binder is computed over. */
static int build_ch(struct tls_sess *s, uint8_t *ch, int max,
                    int *trunc_len, int *binder_off)
{
    int hl = 0; while (s->host[hl]) hl++;
    int n = 0;
    *trunc_len = 0; *binder_off = 0;
    /* Everything up to the ALPN extension is fixed-size apart from the host
     * name, so one check here covers all of it; the two variable-length
     * extensions after it (ALPN, cookie, key_share) check themselves. */
    if (max < 224 + hl) return -1;

    ch[n++] = HS_CLIENT_HELLO; int lenpos = n; n += 3;   /* 3-byte length, filled later */
    ch[n++] = 0x03; ch[n++] = 0x03;                      /* legacy_version */
    memcpy(ch + n, s->random, 32); n += 32;
    ch[n++] = 32; memcpy(ch + n, s->sid, 32); n += 32;   /* legacy_session_id */

    /* cipher_suites: the two TLS 1.3 suites first, then the TLS 1.2 ones. The
     * lists do not compete -- a 1.3 server can only pick from the 0x13xx pair
     * and a 1.2 server only from the rest -- so this is one offer covering both
     * protocols, not a preference ordering between versions (that is what
     * supported_versions below is for). Within 1.2, ChaCha20 leads AES-256
     * because our AES is a byte-at-a-time software S-box while ChaCha20 is
     * add-rotate-xor; on this CPU-emulated target that difference is real.
     * See tls_int.h for why there is no static-RSA or CBC suite here. */
    n += put_u16(ch + n, 18);
    n += put_u16(ch + n, TLS_AES_128_GCM_SHA256);
    n += put_u16(ch + n, TLS_CHACHA20_POLY1305_SHA256);
    /* AES-256-GCM-SHA384 last of the three 1.3 suites, so what we negotiate by
     * default does not change: it is here because some servers PREFER it and
     * a client that never offers it takes whatever else they will accept. */
    n += put_u16(ch + n, TLS_AES_256_GCM_SHA384);
    n += put_u16(ch + n, TLS_ECDHE_ECDSA_CHACHA20_SHA256);
    n += put_u16(ch + n, TLS_ECDHE_RSA_CHACHA20_SHA256);
    n += put_u16(ch + n, TLS_ECDHE_ECDSA_AES128_GCM_SHA256);
    n += put_u16(ch + n, TLS_ECDHE_RSA_AES128_GCM_SHA256);
    n += put_u16(ch + n, TLS_ECDHE_ECDSA_AES256_GCM_SHA384);
    n += put_u16(ch + n, TLS_ECDHE_RSA_AES256_GCM_SHA384);
    ch[n++] = 1; ch[n++] = 0;                            /* compression: null */
    int extlenpos = n; n += 2;

    /* server_name (SNI) */
    n += put_u16(ch + n, EXT_SNI); n += put_u16(ch + n, hl + 5);
    n += put_u16(ch + n, hl + 3); ch[n++] = 0; n += put_u16(ch + n, hl);
    memcpy(ch + n, s->host, (size_t)hl); n += hl;

    /* supported_versions: 1.3 first, then 1.2 -- the order IS the preference,
     * and a 1.3 server takes the first it knows. A 1.2-only server ignores this
     * extension entirely and answers in ServerHello.legacy_version instead,
     * which is why offering 1.2 here costs the 1.3 path nothing. */
    n += put_u16(ch + n, EXT_SUPPORTED_VERS); n += put_u16(ch + n, 5);
    ch[n++] = 4; n += put_u16(ch + n, TLS_V13); n += put_u16(ch + n, TLS_V12);

    /* supported_groups. x25519 is listed first because it is the group we have
     * a constant-time implementation for; the two NIST curves are there so a
     * server that refuses x25519 gets a HelloRetryRequest it can act on
     * instead of a handshake_failure. In TLS 1.2 this same list is what the
     * server picks its ServerKeyExchange curve from. */
    n += put_u16(ch + n, EXT_SUPPORTED_GRPS); n += put_u16(ch + n, 10);
    n += put_u16(ch + n, 8);
    n += put_u16(ch + n, GRP_X25519MLKEM768);
    n += put_u16(ch + n, GRP_X25519);
    n += put_u16(ch + n, GRP_P256);
    n += put_u16(ch + n, GRP_P384);

    /* ec_point_formats = uncompressed. Meaningless in 1.3 (points have one
     * encoding) but RFC 4492 4 requires an ECC-capable 1.2 client to send it,
     * and some 1.2 servers do enforce that. */
    n += put_u16(ch + n, EXT_EC_FORMATS); n += put_u16(ch + n, 2);
    ch[n++] = 1; ch[n++] = 0;

    /* status_request (RFC 6066 8) -- "staple me an OCSP response".
     *
     * CertificateStatusRequest = status_type(1) = ocsp,
     *                            responder_id_list(2) = empty,
     *                            request_extensions(2) = empty.
     * Both lists are empty because both are meaningless to a client that does
     * not run its own OCSP query: naming a responder would only let a server
     * decline, and the one extension worth sending (a nonce) cannot be used
     * with a STAPLED response, which is by construction pre-fetched and shared
     * between connections.
     *
     * Sending this costs 9 bytes and changes nothing when the server does not
     * staple. What it buys is in verify_flight: a revoked certificate now fails
     * the handshake instead of being trusted. See c/net/tls/ocsp.h for the
     * policy, including why a MISSING staple is still accepted. */
    n += put_u16(ch + n, EXT_STATUS_REQUEST); n += put_u16(ch + n, 5);
    ch[n++] = 1;                                        /* status_type = ocsp */
    n += put_u16(ch + n, 0);                            /* responder_id_list */
    n += put_u16(ch + n, 0);                            /* request_extensions */

    /* extended_master_secret (RFC 7627), empty. In TLS 1.2 the plain master
     * secret is derived from the two randoms only, which lets an attacker who
     * can get two different handshakes to agree on the same premaster splice
     * them together (the triple-handshake attack). Binding the master secret to
     * a hash of the whole handshake instead closes that. We ask for it on every
     * 1.2 connection; see tls12.c for what happens when a server declines. */
    n += put_u16(ch + n, EXT_EMS); n += put_u16(ch + n, 0);

    /* renegotiation_info (RFC 5746), empty = "this is an initial handshake and
     * I understand secure renegotiation". Servers hardened against the 2009
     * renegotiation attack reject clients that say nothing at all. We never
     * renegotiate, so an empty extension is the whole of our participation. */
    n += put_u16(ch + n, EXT_RENEG_INFO); n += put_u16(ch + n, 1); ch[n++] = 0;

    /* signature_algorithms. In TLS 1.3 each code point pins a curve as well as
     * a hash; in TLS 1.2 the low byte is only the signature ALGORITHM (3 =
     * ECDSA, 1 = RSA PKCS#1) and the curve comes from the certificate -- so the
     * same list means slightly different things to the two servers, and both
     * readings are covered. The rsa_pkcs1_* entries are here for 1.2's
     * ServerKeyExchange, which commonly signs with PKCS#1 v1.5. */
    n += put_u16(ch + n, EXT_SIG_ALGS); n += put_u16(ch + n, 20); n += put_u16(ch + n, 18);
    n += put_u16(ch + n, 0x0403); n += put_u16(ch + n, 0x0503); n += put_u16(ch + n, 0x0603);
    n += put_u16(ch + n, 0x0804); n += put_u16(ch + n, 0x0805); n += put_u16(ch + n, 0x0806);
    n += put_u16(ch + n, 0x0401); n += put_u16(ch + n, 0x0501); n += put_u16(ch + n, 0x0601);

    /* ALPN */
    int al = put_alpn(s, ch + n, max - n);
    if (al < 0) return -1;
    n += al;

    /* cookie, if the HelloRetryRequest sent one: it must be echoed verbatim
     * (RFC 8446 4.2.2 -- it is how a stateless server recovers the state it
     * refused to keep across the retry). */
    if (s->cookielen) {
        if (n + 6 + s->cookielen > max) return -1;
        n += put_u16(ch + n, EXT_COOKIE); n += put_u16(ch + n, s->cookielen + 2);
        n += put_u16(ch + n, s->cookielen);
        memcpy(ch + n, s->cookie, (size_t)s->cookielen); n += s->cookielen;
    }

    /* psk_key_exchange_modes. Must accompany pre_shared_key, and is written
     * here (not next to it) only because it has no ordering requirement -- the
     * one extension that does is handled below. */
    if (s->psk_offered) {
        int ml = tls_psk_modes_ext(ch + n, max - n);
        if (ml < 0) return -1;
        n += ml;
    }

    /* key_share: exactly one entry, for the group we hold a key for. Offering
     * only x25519 up front (rather than pre-generating a NIST share too) keeps
     * the common handshake cheap -- a P-256 keygen on this bignum is far more
     * expensive than the extra round trip an HRR costs, and the servers that
     * need it are rare. */
    /* One entry normally; TWO when a PQ hybrid is being offered, so that a
     * server without PQ support can answer from the bare x25519 share instead
     * of spending a HelloRetryRequest. The second entry re-uses the x25519
     * public that already sits at the tail of the hybrid share -- see the
     * two-key-share note in tls_int.h for why one scalar serving both offers
     * is sound. */
    int extra = s->group2 ? (4 + HYB_X_LEN) : 0;
    if (n + 12 + s->publen + extra > max) return -1;
    n += put_u16(ch + n, EXT_KEY_SHARE); n += put_u16(ch + n, s->publen + 6 + extra);
    n += put_u16(ch + n, s->publen + 4 + extra);
    n += put_u16(ch + n, s->group); n += put_u16(ch + n, s->publen);
    memcpy(ch + n, s->pub, (size_t)s->publen); n += s->publen;
    if (s->group2) {
        n += put_u16(ch + n, s->group2); n += put_u16(ch + n, HYB_X_LEN);
        memcpy(ch + n, s->pub + HYB_EK_LEN, (size_t)HYB_X_LEN); n += HYB_X_LEN;
    }

    /* pre_shared_key -- RFC 8446 4.2.11 requires it to be the LAST extension,
     * because the binder is an HMAC over the ClientHello truncated at exactly
     * this point. Nothing may be appended after it. */
    if (s->psk_offered) {
        int tr = 0, bo = 0;
        int pl = tls_psk_ext(s, ch + n, max - n, &tr, &bo);
        if (pl < 0) {
            /* No room, or the ticket expired between arming and building.
             * Drop the offer rather than send a pre_shared_key we cannot
             * bind: a malformed one is a handshake failure, a missing one is
             * just a full handshake. */
            s->psk_offered = 0;
        } else {
            *trunc_len  = n + tr;            /* prefix the binder covers */
            *binder_off = n + bo;
            n += pl;
        }
    }

    put_u16(ch + extlenpos, n - extlenpos - 2);
    ch[lenpos] = 0;
    ch[lenpos+1] = (uint8_t)((n - lenpos - 3) >> 8);
    ch[lenpos+2] = (uint8_t)(n - lenpos - 3);
    return n;
}

/* -------------------------------------------------------------- steps */

static int step_send_ch(struct tls_sess *s)
{
    /* 3072, not 2048, and the extra kilobyte is the PQ hybrid's bill.
     *
     * MEASURED, by capturing our own ClientHello off the wire
     * (build/tlsx/chsize_host.sh): the hybrid key_share takes the hello from
     * ~230 B to 1446 B for a 9-character host, growing one byte per SNI
     * character. A resumption pre_shared_key extension needs up to
     *   4 + 2 + 2 + TICKET_BLOB_MAX(512) + 4 + 2 + 1 + 32 = 559 B
     * on top of that. At 2048 the two no longer fit together:
     *   SNI  40 -> 1477 B, headroom 571  -- PSK fits
     *   SNI 100 -> 1537 B, headroom 511  -- PSK DROPPED
     * build_ch drops the offer rather than truncating, so the symptom was not
     * corruption; it was TLS 1.3 resumption silently ceasing to work for any
     * host name over ~52 characters, which is an ordinary length. That is a
     * performance regression that would never have produced an error message.
     *
     * The absolute worst case is a 255-char SNI plus a full ALPN list plus a
     * 512-byte ticket plus the 1216-byte hybrid share, about 2372 B; 3072
     * clears it with room. It costs stack in this frame only, and not on the
     * deepest chain -- the peak during a handshake is the ML-KEM keygen under
     * step_recv_sh (~9.9 KiB), which this function does not nest inside. */
    uint8_t ch[3072];
    int trunc_len = 0, binder_off = 0;
    int n = build_ch(s, ch, (int)sizeof ch, &trunc_len, &binder_off);
    if (n < 0) return fail(s, TLS_E_PROTO);

    /* The binder is computed AFTER the message is complete, because it covers
     * the ClientHello's own length headers -- which build_ch only back-fills
     * once it knows the total. It is also computed against the transcript as it
     * stands BEFORE this message: empty for a first flight, and the synthetic
     * message_hash||HelloRetryRequest state for a retry, which is why a resumed
     * handshake survives an HRR instead of replaying a now-wrong binder. */
    if (binder_off) tls_psk_binder(s, &s->th, ch, trunc_len, binder_off);

    /* The single dummy CCS of RFC 8446 D.4 goes immediately before our second
     * flight -- which is ClientHello2 when there was a retry, and the Finished
     * otherwise. Exactly one, either way. */
    if (s->hrr_seen && !s->ccs_sent) {
        uint8_t ccs = 1;
        if (tx_queue(s, REC_CCS, &ccs, 1)) return fail(s, TLS_E_PROTO);
        s->ccs_sent = 1;
    }
    tls_th_update(s, ch, n);
    if (tx_queue(s, REC_HANDSHAKE, ch, n)) return fail(s, TLS_E_PROTO);
    s->state = TS_RECV_SH;

    int fl = tx_flush(s);
    if (fl < 0) return fail(s, TLS_E_TCP);
    return fl ? TLS_WANT_READ : TLS_WANT_WRITE;
}

/* ServerHello.random sentinel that marks a HelloRetryRequest (RFC 8446 4.1.3). */
static const uint8_t HRR_RANDOM[32] = {
    0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11, 0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
    0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E, 0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C
};

/* Rewrite the transcript for a retry (RFC 8446 4.4.1):
 *      Hash(message_hash || 00 00 <len> || Hash(ClientHello1) || HelloRetryRequest || ...)
 * The real ClientHello1 bytes are *replaced* by a synthetic message carrying
 * their hash, so a stateless server can reconstruct the transcript from the
 * cookie alone without having stored CH1. Getting this wrong does not fail
 * loudly -- it fails at the Finished MAC, which is why it is spelled out. */
static void transcript_restart_for_hrr(struct tls_sess *s)
{
    uint8_t ch1[32], ch1_384[48];
    transcript_hash(&s->th, ch1);
    { struct sha512 c = s->th384; sha384_final(&c, ch1_384); }
    sha256_init(&s->th);
    sha384_init(&s->th384);
    /* Both transcripts are restarted, each with its own hash of ClientHello1.
     * Only the SHA-256 one can ever be used (an HRR means TLS 1.3, and both our
     * 1.3 suites are SHA-256), but leaving the SHA-384 copy holding the
     * pre-retry state would make it silently wrong rather than merely unused. */
    uint8_t hdr[4] = { HS_MESSAGE_HASH, 0, 0, HLEN };
    sha256_update(&s->th, hdr, 4);
    sha256_update(&s->th, ch1, HLEN);
    uint8_t hdr4[4] = { HS_MESSAGE_HASH, 0, 0, 48 };
    sha512_update(&s->th384, hdr4, 4);
    sha512_update(&s->th384, ch1_384, 48);
}

/* What a ServerHello told us, once parsed. */
struct sh_info {
    int suite;
    int version;                             /* from supported_versions, or 0 */
    const uint8_t *spub; int splen;          /* TLS 1.3 key_share */
    int sel_group;                           /* which of our offers it answered */
    int retry_group;                         /* HelloRetryRequest key_share */
    int ems;                                 /* server echoed extended_master_secret */
    const uint8_t *alpn; int alpnlen;        /* TLS 1.2 puts ALPN here, not in EE */
    int psk_selected;                        /* pre_shared_key present: identity index */
};

/* Parse a ServerHello / HelloRetryRequest body into *out. `is_hrr` selects how
 * the key_share extension is read (a retry names a group and sends no key).
 * Every read is bounded by shend, which is bounded by what we received.
 * Returns 0 on a structurally valid message; version negotiation is the
 * caller's decision, not this function's. */
static int parse_sh(struct tls_sess *s, const uint8_t *body, int shend,
                    int is_hrr, struct sh_info *out)
{
    int p = 4 + 2 + 32;                                  /* skip type/len, ver, random */
    int sidlen = body[p++]; p += sidlen;
    if (p + 5 > shend) return -1;                        /* suite(2)+comp(1)+extlen(2) */
    out->suite = (body[p] << 8) | body[p+1]; p += 2;
    /* compression_method. TLS 1.3 requires 0 and TLS 1.2 record compression was
     * removed by RFC 7525 (CRIME); we only ever offer null, so anything else is
     * a server answering an offer we did not make. */
    if (body[p++] != 0) return -1;
    int elen = (body[p] << 8) | body[p+1]; p += 2;
    int eend = p + elen;
    if (eend > shend) return -1;
    while (p + 4 <= eend) {
        int et = (body[p] << 8) | body[p+1], el = (body[p+2] << 8) | body[p+3]; p += 4;
        if (p + el > eend) return -1;
        if (et == EXT_SUPPORTED_VERS && el == 2) {
            out->version = (body[p] << 8) | body[p+1];
        } else if (et == EXT_KEY_SHARE) {
            if (is_hrr) {                                /* HRR: group only, no key */
                if (el != 2) return -1;
                out->retry_group = (body[p] << 8) | body[p+1];
            } else {
                if (el < 4) return -1;
                int grp = (body[p] << 8) | body[p+1];
                int kl = (body[p+2] << 8) | body[p+3];
                /* Must answer ONE OF the offers we actually made. group2 is
                 * nonzero only while a hybrid is on the table, so this cannot
                 * silently widen to a group we never sent a share for. */
                if ((grp != s->group && !(s->group2 && grp == s->group2)) || kl != el - 4)
                    return -1;
                out->sel_group = grp;
                out->spub = body + p + 4; out->splen = kl;
            }
        } else if (et == EXT_COOKIE && is_hrr) {
            if (el < 2) return -1;
            int cl = (body[p] << 8) | body[p+1];
            if (cl != el - 2 || cl > (int)sizeof s->cookie) return -1;
            memcpy(s->cookie, body + p + 2, (size_t)cl);
            s->cookielen = cl;
        } else if (et == EXT_PSK && !is_hrr) {
            /* pre_shared_key in a ServerHello is a bare selected_identity
             * (RFC 8446 4.2.11). We only ever offer ONE identity, so anything
             * other than index 0 is a server selecting an offer we did not
             * make -- refuse rather than resume against the wrong PSK. */
            if (el != 2) return -1;
            int idx = (body[p] << 8) | body[p+1];
            if (idx != 0) return -1;
            out->psk_selected = 1;
        } else if (et == EXT_EMS && el == 0) {
            out->ems = 1;
        } else if (et == EXT_ALPN) {
            out->alpn = body + p; out->alpnlen = el;
        }
        p += el;
    }
    return 0;
}

/* Read the server's ALPN choice out of an ALPN extension body
 * (ProtocolNameList: list_len(2) then one name_len(1)||name). Shared by the
 * 1.3 EncryptedExtensions reader and the 1.2 ServerHello reader. */
void tls_take_alpn(struct tls_sess *s, const uint8_t *ext, int el)
{
    if (el < 3) return;
    int nl = ext[2];
    if (nl > 0 && nl + 3 <= el && nl < (int)sizeof s->alpn_sel) {
        memcpy(s->alpn_sel, ext + 3, (size_t)nl);
        s->alpn_sel[nl] = 0;
    }
}

/* Report a plaintext alert -- these carry the reason a server rejected us
 * (e.g. 40 handshake_failure when no group/suite overlapped), and printing it
 * is the difference between a debuggable failure and "TLS_E_PROTO". */
void tls_log_alert(const uint8_t *body, int blen)
{
    if (blen >= 2) kprintf("[tls] alert level=%d desc=%d\n", body[0], body[1]);
}

static int step_recv_flight(struct tls_sess *s);

static int step_recv_sh(struct tls_sess *s)
{
    /* Loop rather than return: a middlebox-compat CCS is not a reason to tell
     * the caller to wait for more data -- the ServerHello may well be sitting
     * right behind it in the same TCP segment. TLS_WANT_READ must mean "the
     * receive buffer is genuinely dry", or an event loop that only wakes on
     * socket readability deadlocks against bytes it already has. */
    for (;;) {
        int r = rec_pull(s);
        if (r == 0) return TLS_WANT_READ;
        if (r < 0) return fail(s, TLS_E_TCP);
        if (s->rectype == REC_CCS) { rec_drop(s); continue; }
        break;
    }

    const uint8_t *body = s->rxrec + 5;
    int blen = s->reclen;
    if (s->rectype == REC_ALERT) { log_alert(body, blen); rec_drop(s); return fail(s, TLS_E_PROTO); }
    if (s->rectype != REC_HANDSHAKE || blen < 4 || body[0] != HS_SERVER_HELLO)
        return fail(s, TLS_E_PROTO);

    int shlen = (body[1] << 16) | (body[2] << 8) | body[3];
    int shend = 4 + shlen;
    /* Bound the ServerHello to what was received AND to the minimum fixed
     * layout (4 hdr + 2 ver + 32 random + 1 sid-len + 2 suite + 1 comp + 2
     * ext-len) before touching any field. */
    if (blen < 44 || shlen < 40 || shend > blen) return fail(s, TLS_E_PROTO);

    int is_hrr = memcmp(body + 6, HRR_RANDOM, 32) == 0;
    int legacy_version = (body[4] << 8) | body[5];

    if (is_hrr) {
        if (s->hrr_seen) {                      /* RFC 8446 4.1.4: at most one */
            kprintf("[tls] second HelloRetryRequest -- aborting\n");
            return fail(s, TLS_E_PROTO);
        }
        struct sh_info sh; memset(&sh, 0, sizeof sh); sh.retry_group = -1;
        transcript_restart_for_hrr(s);
        tls_th_update(s, body, shend);
        if (parse_sh(s, body, shend, 1, &sh) != 0) return fail(s, TLS_E_PROTO);
        rec_drop(s);
        /* A HelloRetryRequest only exists in TLS 1.3, so it must carry
         * supported_versions saying so; without that we would be acting on a
         * retry from something that never negotiated 1.3. */
        if (sh.version != TLS_V13) return fail(s, TLS_E_PROTO);
        int grp = sh.retry_group;
        /* The retry must ask for a group we offered in supported_groups and
         * did NOT already send a share for; otherwise the server is either
         * confused or trying to make us loop. */
        /* group2 is in the test because a hybrid ClientHello sends TWO shares:
         * a retry naming either of them is asking for something it was already
         * given, which is the loop this check exists to refuse. */
        if (!tls_group_supported13(grp) || grp == s->group ||
            (s->group2 && grp == s->group2)) {
            kprintf("[tls] HRR asked for group 0x%x (unusable) -- aborting\n", grp);
            return fail(s, TLS_E_PROTO);
        }
        if (sh.suite != TLS_AES_128_GCM_SHA256 && sh.suite != TLS_CHACHA20_POLY1305_SHA256 &&
            sh.suite != TLS_AES_256_GCM_SHA384)
            return fail(s, TLS_E_PROTO);
        kprintf("[tls] HelloRetryRequest: %s -> %s\n", group_name(s->group), group_name(grp));
        s->hrr_seen = 1;
        s->group = grp;
        crypto_wipe(s->priv, sizeof s->priv);
        if (gen_share(s) != 0) return fail(s, TLS_E_CRYPTO);
        s->state = TS_SEND_CH;
        return step_send_ch(s);
    }

    /* --- a real ServerHello --- */
    struct sh_info sh; memset(&sh, 0, sizeof sh); sh.retry_group = -1;
    tls_th_update(s, body, shend);
    if (parse_sh(s, body, shend, 0, &sh) != 0) return fail(s, TLS_E_PROTO);

    /* --- version negotiation ---
     * RFC 8446 4.2.1: a TLS 1.3 server signals 1.3 in supported_versions and
     * leaves legacy_version at 0x0303. A TLS 1.2 server has never heard of
     * supported_versions, ignores ours, and answers in legacy_version. So the
     * extension wins where present and legacy_version decides otherwise. */
    int version = sh.version ? sh.version : legacy_version;
    if (version != TLS_V13 && version != TLS_V12) {
        kprintf("[tls] server chose version 0x%x -- we speak only 1.2 and 1.3\n", version);
        return fail(s, TLS_E_PROTO);
    }
    /* Downgrade protection (RFC 8446 4.1.3). A server that KNOWS about TLS 1.3
     * stamps the last 8 bytes of ServerHello.random with a sentinel whenever it
     * negotiates something older. We offer 1.3 in every ClientHello, so such a
     * server would have picked 1.3 if it were really talking to us -- seeing
     * the sentinel means the version was pushed down in flight, and the whole
     * point of the sentinel is that the stamp is inside the signed/MACed
     * handshake where an attacker cannot remove it. A genuinely 1.2-only
     * server does not set it, which is exactly the case we now support. */
    if (version == TLS_V12) {
        static const uint8_t d12[8] = {0x44,0x4F,0x57,0x4E,0x47,0x52,0x44,0x01};
        static const uint8_t d11[8] = {0x44,0x4F,0x57,0x4E,0x47,0x52,0x44,0x00};
        if (memcmp(body + 6 + 24, d12, 8) == 0 || memcmp(body + 6 + 24, d11, 8) == 0) {
            kprintf("[tls] TLS 1.3 downgrade sentinel in a 1.2 ServerHello -- aborting\n");
            return fail(s, TLS_E_PROTO);
        }
    }
    s->version = version;

    if (version == TLS_V12) {
        /* TLS 1.2 puts ALPN in the (cleartext) ServerHello rather than in 1.3's
         * encrypted EncryptedExtensions. Same selection, different envelope. */
        if (sh.alpn) tls_take_alpn(s, sh.alpn, sh.alpnlen);
        memcpy(s->srandom, body + 6, 32);
        s->suite = sh.suite;
        s->ems = sh.ems;
        /* TLS 1.2 packs as many handshake messages into a record as will fit,
         * so this record very often continues straight into Certificate,
         * ServerKeyExchange and ServerHelloDone. Dropping it whole -- which is
         * safe in 1.3, where everything after the ServerHello is encrypted and
         * therefore in a different record -- would silently discard the
         * certificate and leave us waiting for a flight that already arrived.
         * Hand the remainder to the 1.2 flight buffer first. */
        int rest = blen - shend;
        if (rest > 0) {
            if (rest > (int)sizeof s->hsbuf) return fail(s, TLS_E_PROTO);
            tls_th_update(s, body + shend, rest);
            memcpy(s->hsbuf, body + shend, (size_t)rest);
            s->hslen = rest;
        }
        rec_drop(s);
        return tls12_begin(s);
    }

    /* --- TLS 1.3 from here on --- */
    int suite = sh.suite, splen = sh.splen;
    const uint8_t *spub = sh.spub;
    if (!spub || splen < 1) return fail(s, TLS_E_PROTO);
    if (suite != TLS_CHACHA20_POLY1305_SHA256 && suite != TLS_AES_128_GCM_SHA256 &&
        suite != TLS_AES_256_GCM_SHA384)
        return fail(s, TLS_E_PROTO);
    /* BEFORE the key schedule and before tls_th_hash is called for the first
     * time. Both transcripts have been running since the first ClientHello
     * byte; this is what selects which one is the handshake's. */
    s->hashlen = tls13_suite_hash(suite);

    /* --- did the server take our ticket? (RFC 8446 4.2.11) ---
     * Three cases, and only the first two are legal:
     *   offered + selected -> resume; the key schedule starts from the PSK.
     *   offered + ignored  -> full handshake. Normal, and the reason resumption
     *                         can never be a reachability regression: a server
     *                         that rotated its ticket key just does not answer.
     *   NOT offered + selected -> the server is answering an offer we never
     *                         made. Refuse: continuing would mean deriving keys
     *                         from a PSK slot that holds zeros. */
    if (sh.psk_selected && !s->psk_offered) {
        kprintf("[tls] ServerHello selected a PSK we did not offer -- aborting\n");
        return fail(s, TLS_E_PROTO);
    }
    s->psk_accepted = sh.psk_selected && s->psk_offered;
    if (s->psk_accepted) {
        /* RFC 8446 4.2.11: the selected suite MUST have the same hash as the
         * one the ticket was issued under. Both our 1.3 suites are SHA-256, so
         * this can only trip on a server confusing itself -- but the failure it
         * would otherwise cause (a Finished MAC mismatch 300 lines later) is
         * not one anybody would trace back to here. */
        /* Now a real comparison rather than a list of the two suites that
         * happened to share a hash. The binder we sent is an HMAC at the
         * TICKET's hash length, so a server selecting a suite with a different
         * hash has already invalidated it -- and only SHA-256 tickets are ever
         * stored (tls_psk_store refuses the rest), so a 0x1302 selection
         * against an offered PSK lands here. */
        if (tls13_suite_hash(s->psk_suite) != tls13_suite_hash(suite)) {
            kprintf("[tls] resumption suite mismatch -- aborting\n");
            return fail(s, TLS_E_PROTO);
        }
    } else if (s->psk_offered) {
        /* The binder was refused or the ticket was stale. Drop it: re-offering
         * it on every future connection to this host costs a rejected binder
         * each time and never starts working again. */
        kprintf("[tls] ticket for %s not accepted -- full handshake\n", s->host);
        tls_psk_forget(s->host);
        crypto_wipe(s->psk, sizeof s->psk);
    }
    s->suite = suite;

    /* Commit to whichever of our two offers the server answered, BEFORE any key
     * material is derived -- tls_compute_shared dispatches on s->group, so
     * leaving it at the hybrid after the server picked bare x25519 would
     * decapsulate a 32-byte share as if it were a 1120-byte one. parse_sh has
     * already refused anything that is not one of the groups we offered. */
    if (sh.sel_group && sh.sel_group != s->group) {
        s->group  = sh.sel_group;
        s->group2 = 0;
    }

    /* Copy the peer's share out before rec_drop() compacts the receive buffer:
     * spub points INTO s->rxrec, and dropping the record memmoves everything
     * behind it down over exactly those bytes. Reading it afterwards is not a
     * crash -- it silently agrees on the wrong secret, which then surfaces
     * hundreds of lines later as a Finished MAC mismatch. */
    uint8_t speer[TLS_KX_PEER_MAX];
    if (splen > (int)sizeof speer) return fail(s, TLS_E_PROTO);
    memcpy(speer, spub, (size_t)splen);
    rec_drop(s);

    /* --- key schedule: handshake secrets --- */
    uint8_t shared[TLS_KX_SS_MAX]; int sharedlen = 0;
    if (compute_shared(s, speer, splen, shared, &sharedlen) != 0) {
        crypto_wipe(shared, sizeof shared);
        return fail(s, TLS_E_CRYPTO);
    }
    TLSPROF_BEGIN(tls_sched13);
#ifdef LOGIT_TLS13_BREAK_HASH32
    /* NEGATIVE CONTROL (test-gcm256-control). Pin the key schedule at SHA-256
     * width regardless of the suite -- the single most likely way to get
     * TLS_AES_256_GCM_SHA384 wrong, and the reason tls13_suite_hash() exists.
     * It is invisible to every AES-128 and ChaCha20 case (for those, 32 IS the
     * answer); only a SHA-384 handshake notices, and it notices at the server's
     * Finished MAC rather than anywhere near here. */
    const int hl = 32;
#else
    const int hl = s->hashlen;
#endif
    uint8_t zeros[48]; memset(zeros, 0, sizeof zeros);
    uint8_t early[48], derived[48], emptyhash[48];
    /* The early secret is the ONLY place resumption changes the key schedule:
     * HKDF-Extract over the PSK instead of over zeros (RFC 8446 7.1). Every
     * derivation below is byte-identical either way -- which is why a resumed
     * handshake that got this one line wrong fails at the server's Finished
     * MAC and nowhere earlier. */
    if (s->psk_accepted) hkdf_extract(hl, 0, 0, s->psk, HLEN, early);
    else                 hkdf_extract(hl, 0, 0, zeros, hl, early);
    empty_hash(hl, emptyhash);
    derive_secret(hl, early, "derived", emptyhash, derived);
    hkdf_extract(hl, derived, hl, shared, sharedlen, s->sec.hs);
    uint8_t th_chsh[48]; tls_th_hash(s, th_chsh);
    derive_secret(hl, s->sec.hs, "s hs traffic", th_chsh, s->sec.s_hs);
    derive_secret(hl, s->sec.hs, "c hs traffic", th_chsh, s->sec.c_hs);
    traffic_keys(s->sec.s_hs, suite, &s->cr);
    traffic_keys(s->sec.c_hs, suite, &s->cw);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(early, sizeof early);
    crypto_wipe(derived, sizeof derived);
    TLSPROF_END(tls_sched13);

    kprintf("[tls] ServerHello: TLS 1.3, suite 0x%x (%s), group %s%s\n",
            suite,
            suite == TLS_AES_128_GCM_SHA256 ? "AES-128-GCM-SHA256"
          : suite == TLS_AES_256_GCM_SHA384 ? "AES-256-GCM-SHA384"
                                            : "CHACHA20-POLY1305-SHA256",
            group_name(s->group), s->hrr_seen ? " (after HRR)" : "");
    s->state = TS_RECV_FLIGHT;
    /* Servers send the whole flight back-to-back, so EncryptedExtensions is
     * usually already in the buffer. Carry straight on instead of reporting
     * WANT_READ for data we are holding. */
    return step_recv_flight(s);
}

/* Verify a parsed chain against the trust store, and say WHY if it is refused.
 * "TLS_E_CERT" is indistinguishable between an expired leaf, a name mismatch
 * and an anchor we do not hold, and those three call for completely different
 * responses from whoever is looking. Shared by both protocol versions -- the
 * certificate is the one thing TLS 1.2 and 1.3 agree about. */
int tls_check_chain(struct tls_sess *s, const struct cert *chain, int ncert)
{
    if (ncert < 1) { kprintf("[tls] no usable certificate in the flight\n"); return TLS_E_CERT; }
    TLSPROF_BEGIN(tls_chain_verify);
    int vr = x509_verify_chain(chain, ncert, s->host, s->now);
    TLSPROF_END(tls_chain_verify);
    if (vr != X509_OK) {
        kprintf("[tls] chain of %d rejected for %s: %s (%d)\n", ncert, s->host,
                vr == X509_E_NAME ? "host name" : vr == X509_E_EXPIRED ? "validity dates"
                : vr == X509_E_UNTRUSTED ? "no path to a trusted root"
                : vr == X509_E_SIG ? "bad signature" : "parse", vr);
        return TLS_E_CERT;
    }
    kprintf("[tls] chain of %d verified for %s%s%s\n", ncert, s->host,
            s->alpn_sel[0] ? ", alpn=" : "", s->alpn_sel[0] ? s->alpn_sel : "");
    return 0;
}

/* The stapled-OCSP decision, shared by both protocol versions (TLS 1.3 carries
 * the response in a CertificateEntry extension, TLS 1.2 in a CertificateStatus
 * message; the bytes and the verdict are identical).
 *
 * Read c/net/tls/ocsp.h for the policy. The asymmetry implemented here is the
 * whole of it: a MISSING staple is fine, a PRESENT one that does not check out
 * is fatal. Anything else lets an attacker turn "revoked" into "absent" by
 * corrupting a byte.
 *
 * The one case that is neither: a flight with no issuer certificate in it. An
 * OCSP CertID is hashes of the ISSUER's name and key, so without the issuer
 * there is nothing to match against -- we cannot say the response is good and
 * we cannot say it is bad. That is logged and the handshake continues, which is
 * the same treatment as no staple at all, because it is the same amount of
 * information. It happens only for a leaf signed directly by a root, which no
 * public CA does. */
int tls_check_staple(struct tls_sess *s, const struct cert *chain, int ncert,
                     const uint8_t *staple, int staplelen)
{
#ifdef LOGIT_OCSP_BREAK_IGNORE
    /* NEGATIVE CONTROL (test-ocsp-control). Accept whatever was stapled without
     * looking at it -- which is not a strawman, it is what this stack did up to
     * now and what a client that merely SENDS status_request does. Every other
     * test in the tree is blind to it: the chain still verifies, the handshake
     * still completes, the site still loads. The only case that can see it is
     * the one where the response says REVOKED, which is why that case exists. */
    (void)chain; (void)ncert; (void)staple; (void)staplelen;
    return 0;
#endif
    if (!staple || staplelen <= 0) return 0;          /* no staple: proceed */
    if (ncert < 2) {
        kprintf("[tls] %s stapled an OCSP response but sent no issuer -- not checked\n", s->host);
        return 0;
    }
    /* 300 s of skew: this machine's clock is a CMOS RTC read at boot, and a
     * response that is five minutes out of step is a clock problem, not a
     * revocation. */
    int rc = ocsp_check(staple, staplelen, &chain[0], &chain[1], s->now, 300);
    if (rc == OCSP_OK) {
        kprintf("[tls] OCSP staple: good (%s)\n", s->host);
        return 0;
    }
    kprintf("[tls] OCSP staple REFUSED for %s: %s (%d)\n", s->host, ocsp_strerror(rc), rc);
    return TLS_E_CERT;
}

/* Walk the buffered flight and verify it. 0 ok, negative TLS_E_*. */
static int verify_flight(struct tls_sess *s)
{
    /* static, not automatic: 8 parsed certificates are ~1 KiB that the kernel
     * stack should not carry. Safe with several live sessions for the same
     * reason aead_seal's staging buffer is -- verify_flight runs start to
     * finish inside one tls_step() call and never yields. */
    static struct cert chain[8];
    int ncert = 0;
    /* The stapled OCSP response, if the server sent one. It points INTO
     * s->hsbuf, which outlives this function's use of it. */
    const uint8_t *staple = 0; int staplelen = 0;
    /* Hash through Certificate. Zeroed so that a CertificateVerify arriving
     * before any Certificate fails verification rather than reading garbage. */
    uint8_t th_cert[48] = { 0 };
    /* Did a CertificateVerify with a VERIFIED signature actually arrive?
     *
     * This loop is a dispatch over whatever messages the server chose to send,
     * not a state machine, so every check below is conditional on the message
     * that carries it being present. The signature check lives inside
     * `if (mt == HS_CERT_VERIFY)`; omit that message and the branch simply
     * never runs. Nothing after the loop noticed: tls_check_chain proves the
     * certificate is AUTHENTIC (chains to a root, matches the host, in date)
     * and says nothing about whether the peer holds its private key -- which
     * is the one thing CertificateVerify exists to prove.
     *
     * Certificates are public. They are sent in the clear on every real
     * connection and mirrored in CT logs, so "hold the target's certificate"
     * is not an attacker capability, it is a download. An on-path attacker
     * could therefore replay any site's real chain, skip the signature, and
     * this client printed "chain of N verified for <host>" and proceeded --
     * measured on 2026-08-20 by tests/unit/run-tls13-certverify-bypass-probe.sh,
     * which watched exactly that happen against a genuine chain.
     *
     * RFC 8446 4.4.3: the message is mandatory in every handshake that
     * authenticates with a certificate. Recording presence rather than
     * reordering the loop into a real state machine is deliberate -- the state
     * machine is the right long-term shape and is a much larger change (see
     * the report's state-machine section); this closes the authentication hole
     * without touching the parse of any message. */
    int cv_ok = 0;
    const uint8_t *flight = s->hsbuf;
    int flen = s->hslen;
    int q = 0;

    while (q + 4 <= flen) {
        int mt = flight[q];
        int ml = (flight[q+1] << 16) | (flight[q+2] << 8) | flight[q+3];
        if (q + 4 + ml > flen) break;                    /* ignore an incomplete tail */
        const uint8_t *mb = flight + q + 4;

        if (mt == HS_ENCRYPTED_EXT) {
            tls_th_update(s, flight + q, 4 + ml);
            /* EncryptedExtensions is where the server reports its ALPN choice. */
            int p = 0;
            if (ml >= 2) {
                int elen = (mb[0] << 8) | mb[1]; p = 2;
                int eend = p + elen;
                if (eend <= ml) {
                    while (p + 4 <= eend) {
                        int et = (mb[p] << 8) | mb[p+1], el = (mb[p+2] << 8) | mb[p+3]; p += 4;
                        if (p + el > eend) break;
                        if (et == EXT_ALPN) tls_take_alpn(s, mb + p, el);
                        p += el;
                    }
                }
            }
        } else if (mt == HS_CERTIFICATE) {
            tls_th_update(s, flight + q, 4 + ml);
            /* cert_request_ctx(1) + cert_list(3) of {cert(3) + exts(2)}. Every
             * length is bounded by ml, so a crafted Certificate cannot read
             * past the handshake message. */
            if (ml < 4) return TLS_E_PROTO;
            int cp = 1 + mb[0];
            if (cp + 3 > ml) return TLS_E_PROTO;
            int listlen = (mb[cp] << 16) | (mb[cp+1] << 8) | mb[cp+2]; cp += 3;
            int cend = cp + listlen;
            if (cend > ml) return TLS_E_PROTO;
            int entry = 0;
            while (cp + 3 <= cend && ncert < 8) {
                int clen = (mb[cp] << 16) | (mb[cp+1] << 8) | mb[cp+2]; cp += 3;
                if (cp + clen + 2 > cend) return TLS_E_PROTO;
                TLSPROF_BEGIN(tls_cert_parse);
                int pr = x509_parse(mb + cp, clen, &chain[ncert]);
                TLSPROF_END(tls_cert_parse);
                if (pr == 0) ncert++;
                cp += clen;
                int extl = (mb[cp] << 8) | mb[cp+1]; cp += 2;
                if (cp + extl > cend) return TLS_E_PROTO;
                /* CertificateEntry extensions. TLS 1.3 moved the stapled OCSP
                 * response HERE, per-certificate (RFC 8446 4.4.2.1), rather
                 * than into the separate CertificateStatus message TLS 1.2
                 * uses -- so a 1.3 client that only knows the 1.2 shape sees no
                 * staple on any connection and never notices. Only the FIRST
                 * entry's is read: a response about an intermediate says
                 * nothing about whether the leaf's key was stolen, and the leaf
                 * is the certificate whose revocation matters. */
                if (entry == 0 && extl >= 4) {
                    int ep = cp, eend = cp + extl;
                    while (ep + 4 <= eend) {
                        int et = (mb[ep] << 8) | mb[ep+1];
                        int el = (mb[ep+2] << 8) | mb[ep+3];
                        ep += 4;
                        if (ep + el > eend) break;
                        /* CertificateStatus: status_type(1) + response<1..2^24-1> */
                        if (et == EXT_STATUS_REQUEST && el >= 4 && mb[ep] == 1) {
                            int rl = (mb[ep+1] << 16) | (mb[ep+2] << 8) | mb[ep+3];
                            if (rl > 0 && 4 + rl <= el) {
                                staple = mb + ep + 4;
                                staplelen = rl;
                            }
                        }
                        ep += el;
                    }
                }
                cp += extl;
                entry++;
            }
            tls_th_hash(s, th_cert);
        } else if (mt == HS_CERT_VERIFY) {
            /* signature over 64*0x20 || "TLS 1.3, server CertificateVerify" || 0 || th_cert */
            if (ml < 4) return TLS_E_PROTO;
            int sigalg = (mb[0] << 8) | mb[1];
            int siglen = (mb[2] << 8) | mb[3];
            if (4 + siglen > ml) return TLS_E_PROTO;
            const uint8_t *sig = mb + 4;
            uint8_t signed_data[64 + 33 + 1 + 48]; int sd = 0;
            for (int i = 0; i < 64; i++) signed_data[sd++] = 0x20;
            const char *ctx = "TLS 1.3, server CertificateVerify";
            for (int i = 0; ctx[i]; i++) signed_data[sd++] = (uint8_t)ctx[i];
            signed_data[sd++] = 0;
            memcpy(signed_data + sd, th_cert, (size_t)s->hashlen); sd += s->hashlen;
            int okv = 0;
            TLSPROF_BEGIN(tls_certverify);
            /* ecdsa_secp521r1_sha512 (0x0603) is handled here because we OFFER
             * it in signature_algorithms. It used to be advertised and not
             * implemented, so a server with a P-521 certificate took us at our
             * word, signed with it, and the handshake died right here -- the
             * site was unreachable and it looked like the server's fault. */
#ifdef LOGIT_P521_BREAK_FLEN
            /* NEGATIVE CONTROL (see test-p521-control). Use curve/8 for the
             * field length instead of ceil(curve/8). It is right for P-256 and
             * P-384 and wrong only for P-521, by one byte -- the exact mistake
             * this curve invites, and one that changes nothing anywhere else. */
#           define x509_ec_flen(c) ((c) / 8)
#endif
            if (ncert > 0 && (sigalg == 0x0403 || sigalg == 0x0503 || sigalg == 0x0603)) {
                int curve = (sigalg == 0x0403) ? 256 : (sigalg == 0x0503) ? 384 : 521;
                int flen2 = x509_ec_flen(curve);      /* 66 for P-521, not 65 */
                uint8_t hash[64]; int hh;
                if (curve == 256)      { sha256(signed_data, sd, hash); hh = 32; }
                else if (curve == 384) { sha384(signed_data, sd, hash); hh = 48; }
                else                   { sha512(signed_data, sd, hash); hh = 64; }
                uint8_t rs[132];
                if (chain[0].key_type == KEY_EC && x509_der_sig_to_rs(sig, siglen, rs, flen2) == 0 &&
                    chain[0].publen == 1 + 2*flen2 && chain[0].pub[0] == 0x04 &&
                    ecdsa_verify(curve, chain[0].pub + 1, rs, hash, hh)) okv = 1;
            } else if (ncert > 0 && (sigalg == 0x0804 || sigalg == 0x0805 || sigalg == 0x0806) &&
                       chain[0].key_type == KEY_RSA) {
                int hh = (sigalg == 0x0804) ? 32 : (sigalg == 0x0805) ? 48 : 64;
                uint8_t hash[64];
                if (hh == 32) sha256(signed_data, sd, hash);
                else if (hh == 48) sha384(signed_data, sd, hash);
                else sha512(signed_data, sd, hash);
                if (rsa_pss_verify(chain[0].rsa_n, chain[0].rsa_nlen, chain[0].rsa_e, chain[0].rsa_elen,
                                   sig, siglen, hash, hh)) okv = 1;
            }
            TLSPROF_END(tls_certverify);
            if (!okv) {
                kprintf("[tls] CertificateVerify rejected (sigalg 0x%x, %d certs)\n", sigalg, ncert);
                return TLS_E_CERT;
            }
            /* Set only here, after okv -- i.e. only a signature that VERIFIED
             * counts as presence. Setting it on entry to the branch would make
             * a malformed CertificateVerify satisfy the post-loop gate, which
             * is the same hole one message along. */
            cv_ok = 1;
            tls_th_update(s, flight + q, 4 + ml);
        } else if (mt == HS_FINISHED) {
            const int hl = s->hashlen;
            uint8_t th_cv[48]; tls_th_hash(s, th_cv);            /* through CertVerify */
            uint8_t fk[48], expect[48];
            hkdf_expand_label(hl, s->sec.s_hs, "finished", 0, 0, fk, hl);
            hmac(hl, fk, hl, th_cv, hl, expect);
            int bad = (ml != hl) || memcmp(expect, mb, (size_t)hl) != 0;
            crypto_wipe(fk, sizeof fk); crypto_wipe(expect, sizeof expect);
            if (bad) return TLS_E_CRYPTO;
            tls_th_update(s, flight + q, 4 + ml);
        }
        q += 4 + ml;
    }

    /* --- who authenticated this server? ---
     * On a full handshake it is the certificate chain, and tls_check_chain is
     * the whole of our trust decision. On a RESUMED handshake there is no
     * chain: the server proved it holds the PSK by accepting our binder and
     * then producing a Finished under keys derived from it, and that PSK came
     * out of a previous handshake whose chain WAS verified. So the check is
     * skipped -- but only because s->psk_accepted was set by a ServerHello
     * that echoed an identity we ourselves offered.
     *
     * The strict half matters as much: a resumed handshake MUST NOT carry a
     * Certificate (RFC 8446 4.4.2). Accepting one would create a second,
     * unverified way for a server to present itself, which is precisely the
     * kind of "extra path that skips the check" this file exists to not have.
     * A full handshake with no certificate is caught by tls_check_chain's own
     * ncert < 1 refusal, so both directions are covered. */
    if (s->psk_accepted) {
        if (ncert > 0) {
            kprintf("[tls] resumed handshake sent a Certificate -- aborting\n");
            return TLS_E_PROTO;
        }
        kprintf("[tls] resumed session for %s (no chain, no signature)%s%s\n", s->host,
                s->alpn_sel[0] ? ", alpn=" : "", s->alpn_sel[0] ? s->alpn_sel : "");
        return 0;
    }

    /* A full handshake MUST have carried a verified CertificateVerify. Checked
     * BEFORE tls_check_chain so the refusal names the real defect: a chain that
     * verifies is exactly what an attacker replaying a public certificate
     * presents, so letting the chain check run first would print "chain
     * verified" and then reject, which reads like a chain problem.
     *
     * Deliberately NOT guarded by a -D switch. The negative control for this
     * gate is the probe's own honest-but-garbage-signature case, which is a
     * property of the server it talks to rather than a build of ours; a
     * compile-time way to remove an authentication check is a second, quieter
     * way to ship without one. */
    if (!cv_ok) {
        kprintf("[tls] no CertificateVerify from %s -- the peer never proved it "
                "holds the certificate's private key; aborting\n", s->host);
        return TLS_E_CERT;
    }

    {
        int cr = tls_check_chain(s, chain, ncert);
        if (cr != 0) return cr;
    }
    return tls_check_staple(s, chain, ncert, staple, staplelen);
}

/* Does the buffered flight end with a complete Finished? */
static int flight_complete(const struct tls_sess *s)
{
    int q = 0;
    while (q + 4 <= s->hslen) {
        int mt = s->hsbuf[q];
        int ml = (s->hsbuf[q+1] << 16) | (s->hsbuf[q+2] << 8) | s->hsbuf[q+3];
        if (q + 4 + ml > s->hslen) return 0;
        if (mt == HS_FINISHED) return 1;
        q += 4 + ml;
    }
    return 0;
}

static int step_recv_flight(struct tls_sess *s)
{
    while (!flight_complete(s)) {
        int r = rec_pull(s);
        if (r == 0) return TLS_WANT_READ;
        if (r < 0) return fail(s, TLS_E_TCP);
        const uint8_t *body = s->rxrec + 5;
        int blen = s->reclen;
        if (s->rectype == REC_CCS) { rec_drop(s); continue; }   /* middlebox compat */
        if (s->rectype == REC_ALERT) { log_alert(body, blen); rec_drop(s); return fail(s, TLS_E_PROTO); }
        if (s->rectype != REC_APPDATA) return fail(s, TLS_E_PROTO);
        if (blen - 16 > (int)sizeof s->app) return fail(s, TLS_E_PROTO);
        uint8_t it;
        TLSPROF_BEGIN(tls_hs_aead);
        int dl = aead_open(&s->cr, body, blen, s->app, &it);
        TLSPROF_END(tls_hs_aead);
        rec_drop(s);
        if (dl < 0) return fail(s, TLS_E_CRYPTO);
        if (it == REC_ALERT) { log_alert(s->app, dl); return fail(s, TLS_E_PROTO); }
        if (it != REC_HANDSHAKE) continue;
        if (s->hslen + dl > (int)sizeof s->hsbuf) return fail(s, TLS_E_PROTO);
        memcpy(s->hsbuf + s->hslen, s->app, (size_t)dl);
        s->hslen += dl;
    }

    int rc = verify_flight(s);
    if (rc) return fail(s, rc);

    /* --- client Finished --- */
    const int hl = s->hashlen;
    uint8_t th_full[48]; tls_th_hash(s, th_full);
    uint8_t cfk[48], cverify[48];
    hkdf_expand_label(hl, s->sec.c_hs, "finished", 0, 0, cfk, hl);
    hmac(hl, cfk, hl, th_full, hl, cverify);
    uint8_t fin[4 + 48]; int finlen = 4 + hl;
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = (uint8_t)hl;
    memcpy(fin + 4, cverify, (size_t)hl);
    crypto_wipe(cfk, sizeof cfk); crypto_wipe(cverify, sizeof cverify);

    if (!s->ccs_sent) {                                  /* RFC 8446 D.4, see step_send_ch */
        uint8_t ccs = 1;
        if (tx_queue(s, REC_CCS, &ccs, 1)) return fail(s, TLS_E_PROTO);
        s->ccs_sent = 1;
    }
    /* 96, not 64: a SHA-384 Finished is 52 bytes of handshake message and the
     * sealed record adds the inner content type plus a 16-byte tag. At 64 the
     * SHA-256 case fitted with 11 bytes to spare and the SHA-384 case would
     * have overflowed a kernel stack buffer. */
    uint8_t finrec[96];
    int frl = aead_seal(&s->cw, REC_HANDSHAKE, fin, finlen, finrec);
    if (frl < 0 || tx_queue(s, REC_APPDATA, finrec, frl)) return fail(s, TLS_E_PROTO);

    /* --- application traffic secrets --- */
    uint8_t emptyhash[48], derived2[48], master[48], c_ap[48], s_ap[48], zeros[48];
    memset(zeros, 0, sizeof zeros);
    empty_hash(hl, emptyhash);
    derive_secret(hl, s->sec.hs, "derived", emptyhash, derived2);
    hkdf_extract(hl, derived2, hl, zeros, hl, master);
    derive_secret(hl, master, "c ap traffic", th_full, c_ap);
    derive_secret(hl, master, "s ap traffic", th_full, s_ap);
    traffic_keys(c_ap, s->suite, &s->cw);
    traffic_keys(s_ap, s->suite, &s->cr);

    /* --- resumption_master_secret (RFC 8446 7.1) ---
     * Note the transcript: the application traffic secrets above are keyed on
     * ClientHello..server Finished, but res master is keyed on
     * ClientHello..CLIENT Finished -- one message further. So the client
     * Finished has to go into the transcript first, and the two hashes are
     * genuinely different values. Deriving res master from th_full instead
     * produces a PSK the server will never agree with, and the only symptom is
     * that every resumption attempt quietly falls back to a full handshake --
     * i.e. it looks exactly like not having implemented this at all. */
#ifdef LOGIT_PSK_BREAK_TRANSCRIPT
    /* NEGATIVE CONTROL (never defined in a real build; see the
     * test-tls-resume-control target). Derive res master from the transcript
     * WITHOUT the client Finished -- the single most plausible way to get this
     * wrong. The handshake still completes, the ticket is still stored, and
     * every existing test still passes; only the resumption assertions notice.
     * If the suite goes green with this defined, the resumption tests are not
     * testing resumption. */
    tls_th_update(s, fin, finlen);
    derive_secret(hl, master, "res master", th_full, s->res_master);
#else
    tls_th_update(s, fin, finlen);
    uint8_t th_res[48]; tls_th_hash(s, th_res);
    derive_secret(hl, master, "res master", th_res, s->res_master);
#endif
    s->res_valid = 1;

    crypto_wipe(derived2, sizeof derived2); crypto_wipe(master, sizeof master);
    crypto_wipe(c_ap, sizeof c_ap); crypto_wipe(s_ap, sizeof s_ap);
    /* the handshake secrets have done their job; only the traffic keys remain */
    crypto_wipe(&s->sec, sizeof s->sec);
    crypto_wipe(s->priv, sizeof s->priv);
    s->hslen = 0;

    s->state = TS_FIN_FLUSH;
    int fl = tx_flush(s);
    if (fl < 0) return fail(s, TLS_E_TCP);
    if (!fl) return TLS_WANT_WRITE;
    s->state = TS_ESTABLISHED;
    tlsprof_close(&s->prof);
    return TLS_DONE;
}

/* ------------------------------------------------------------- public API */

/* Say out loud, once, WHAT this machine trusts -- the two counts, and then
 * every root by name.
 *
 * The counts alone were the whole banner until logit_root_names[] existed, and
 * they were never the interesting half: tools/roots/ holding 130 PEMs means
 * nothing if some of them never made it into the binary, but "130 roots" also
 * means nothing to someone asking whether a particular CA is in there. A
 * trust store that cannot enumerate itself can only be audited by rebuilding
 * it, which is not an operation a running machine can perform.
 *
 * Called from kernel_main (c/kernel/core/kmain.c, beside net_init) as well as
 * from tls_start below -- `done` makes the second call a no-op, so on an
 * ordinary boot this prints at boot and the handshake-time call finds it
 * already said. That relocation is only safe because tests/boot/run-test.sh
 * greps for the EXACT counts, so a store that quietly shrank is a red test
 * rather than a line that still prints. */
void trust_banner(void)
{
    static int done;
    if (done) return;
    done = 1;
    kprintf("[tls] trust store: %d roots, %d skipped\n", logit_nroots, logit_nroots_skipped);
    /* Packed several to a line: 130 names one per line buries the rest of the
     * boot log, and the names are for reading, not for parsing. */
    char line[96];
    int n = 0;
    for (int i = 0; i < logit_nroots; i++) {
        const char *s = logit_root_names[i];
        int l = 0; while (s[l]) l++;
        if (l > (int)sizeof line - 2) l = (int)sizeof line - 2;  /* never overrun */
        if (n && n + 1 + l > 78) { line[n] = 0; kprintf("[tls]   %s\n", line); n = 0; }
        if (n) line[n++] = ' ';
        for (int k = 0; k < l; k++) line[n++] = s[k];
    }
    if (n) { line[n] = 0; kprintf("[tls]   %s\n", line); }
    for (int i = 0; i < logit_nroots_skipped && logit_roots_skipped[i]; i++)
        kprintf("[tls]   NOT TRUSTED (unsupported key): %s\n", logit_roots_skipped[i]);
}

int tls_start(int tcp_id, const char *host, const char *alpn, int64_t now)
{
    trust_banner();
    /* refuse to key a handshake from the weak rdtsc-only RNG fallback */
    if (!rng_strong()) {
        kprintf("[tls] refusing handshake: weak RNG (no rdrand/rdseed)\n");
        return TLS_E_CRYPTO;
    }
    int hl = 0; while (host[hl]) hl++;
    if (hl < 1 || hl > 255) return TLS_E_PROTO;          /* SNI must fit the ClientHello */

    int id = -1;
    for (int i = 0; i < TLS_MAX_SESSIONS; i++) if (!sessions[i].used) { id = i; break; }
    if (id < 0) return TLS_E_PROTO;
    struct tls_sess *s = &sessions[id];
    memset(s, 0, sizeof *s);
    tlsprof_open(&s->prof, &tls_hs_slot, "tls_handshake");
    s->used = 1; s->tcp = tcp_id; s->now = now;
    /* One absolute budget for the whole handshake (~30 s). A peer trickling one
     * byte at a time must not be able to stretch the wait forever by resetting
     * a per-record idle timer. */
    s->deadline = timer_ticks() + 3000;
    memcpy(s->host, host, (size_t)hl); s->host[hl] = 0;
    if (alpn) {
        int i = 0;
        while (alpn[i] && i < (int)sizeof s->alpn_offer - 1) { s->alpn_offer[i] = alpn[i]; i++; }
        s->alpn_offer[i] = 0;
    }
    rand_bytes(s->random, 32);
    rand_bytes(s->sid, 32);
    s->group = GRP_X25519MLKEM768;
    if (gen_share(s) != 0) { fail(s, TLS_E_CRYPTO); return TLS_E_CRYPTO; }
    s->state = TS_SEND_CH;
    /* hashlen stays 32 until a cipher suite says otherwise; both transcripts
     * run from the very first ClientHello byte because the choice arrives after
     * we have already hashed it. */
    s->hashlen = 32;
    sha256_init(&s->th);
    sha384_init(&s->th384);
    /* Arm resumption last, after the transcript is initialised: tls_psk_arm
     * only loads the PSK, but build_ch computes the binder against s->th and
     * an uninitialised transcript would produce a binder the server rejects.
     * A miss here is not an error -- it is the first visit to this host. */
    tls_psk_arm(s);
    return id;
}

int tls_step(int id)
{
    struct tls_sess *s = sess_of(id);
    if (!s) return TLS_E_PROTO;
    if (s->state == TS_ESTABLISHED) return TLS_DONE;
    if (s->state == TS_FAILED) return s->err;

    if (timer_ticks() >= s->deadline) {
        kprintf("[tls] handshake timed out\n");
        return fail(s, TLS_E_TCP);
    }

    /* Anything already sealed must go out before we do anything else -- the
     * record layer is a byte stream and cannot be reordered. */
    int fl = tx_flush(s);
    if (fl < 0) return fail(s, TLS_E_TCP);
    if (fl == 0) return TLS_WANT_WRITE;

    switch (s->state) {
    case TS_SEND_CH:       return step_send_ch(s);
    case TS_RECV_SH:       return step_recv_sh(s);
    case TS_RECV_FLIGHT:   return step_recv_flight(s);
    case TS_FIN_FLUSH:     s->state = TS_ESTABLISHED; tlsprof_close(&s->prof); return TLS_DONE;
    case TS12_RECV_FLIGHT:
    case TS12_RECV_CCS:    return tls12_step(s);
    default:               return fail(s, TLS_E_PROTO);
    }
}

int tls_connect(int tcp_id, const char *host, int64_t now)
{
    int id = tls_start(tcp_id, host, "http/1.1", now);
    if (id < 0) return id;
    for (;;) {
        int rc = tls_step(id);
        if (rc == TLS_DONE) return id;
        if (rc < 0) { tls_close(id); return rc; }
        /* Blocking mode: we own the pump. tls_step never sleeps, so without
         * these two the loop would spin the CPU and starve the NIC poll that
         * has to deliver the very bytes we are waiting for.
         *
         * This is also the round-trip term of the breakdown: every nanosecond
         * spent here is the network answering, not this machine computing, and
         * no amount of faster crypto moves it. Reading tls_handshake minus
         * tls_netwait is what says whether the handshake is CPU-bound at all. */
        TLSPROF_BEGIN(tls_netwait);
        net_poll();
        net_idle();
        TLSPROF_END(tls_netwait);
    }
}

int tls_send(int id, const void *buf, int len)
{
    struct tls_sess *s = sess_of(id);
    if (!s || s->state != TS_ESTABLISHED || len < 0) return -1;
    int fl = tx_flush(s);
    if (fl < 0) return -1;
    if (fl == 0) return 0;                               /* a record is still going out */
    if (len == 0) return 0;
    int n = len > SEND_REC_MAX ? SEND_REC_MAX : len;
    if (s->version == TLS_V12) {
        if (tls12_write_record(s, REC_APPDATA, buf, n) != 0) return -1;
    } else {
        uint8_t rec[SEND_REC_MAX + 32];
        int rl = aead_seal(&s->cw, REC_APPDATA, buf, n, rec);
        if (rl < 0 || tx_queue(s, REC_APPDATA, rec, rl)) return -1;
    }
    if (tx_flush(s) < 0) return -1;
    /* The record is sealed and owned by the session now; whether TCP took all
     * of it is irrelevant to the caller, who must not resend these bytes. */
    return n;
}

/* Cache any NewSessionTicket messages in a decrypted post-handshake record.
 *
 * One record can hold several concatenated handshake messages, and servers do
 * batch tickets (openssl sends two by default, Google sends two), so this
 * walks the whole buffer rather than looking at the first message. Anything
 * that is not a NewSessionTicket is skipped, and a malformed one stops the
 * walk without failing the connection: a ticket we cannot parse costs us a
 * future full handshake, which is not a reason to drop a working session.
 *
 * Every length below is bounded against `len` before it is used. This parses
 * attacker-controlled bytes in ring 0, so "the server would not do that" is
 * not an argument that appears anywhere in it.
 */
static void take_tickets(struct tls_sess *s, const uint8_t *p, int len)
{
    if (!s->res_valid) return;               /* no resumption secret to key from */
    int q = 0;
    while (q + 4 <= len) {
        int mt = p[q];
        int ml = (p[q+1] << 16) | (p[q+2] << 8) | p[q+3];
        if (ml < 0 || q + 4 + ml > len) return;          /* truncated: stop */
        if (mt != HS_NEW_TICKET) { q += 4 + ml; continue; }

        const uint8_t *b = p + q + 4;
        int i = 0;
        if (ml < 4 + 4 + 1) return;
        uint32_t lifetime = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                            ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
        uint32_t age_add  = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                            ((uint32_t)b[6] << 8)  |  (uint32_t)b[7];
        i = 8;
        int nl = b[i++];                                 /* ticket_nonce<0..255> */
        if (i + nl > ml) return;
        const uint8_t *nonce = b + i; i += nl;
        if (i + 2 > ml) return;
        int tl = (b[i] << 8) | b[i+1]; i += 2;           /* ticket<1..2^16-1> */
        if (tl < 1 || i + tl > ml) return;
        const uint8_t *blob = b + i; i += tl;
        /* extensions<0..2^16-2>: the only one defined for a ticket is
         * early_data, which we neither offer nor accept (see the 0-RTT note in
         * tls.h), so the block is bounds-checked and skipped. */
        if (i + 2 > ml) return;
        int xl = (b[i] << 8) | b[i+1]; i += 2;
        if (i + xl > ml) return;

        tls_psk_store(s->host, s->suite, s->res_master, nonce, nl,
                      blob, tl, lifetime, age_add, s->now);
        q += 4 + ml;
    }
}

int tls_recv(int id, void *buf, int max)
{
    struct tls_sess *s = sess_of(id);
    if (!s || max <= 0) return -1;
    if (s->state == TS_FAILED) return -1;
    if (s->state != TS_ESTABLISHED) return 0;

    /* Keep the send side moving. A sealed record that TCP would not take is
     * held in the session, and the only other thing that retries it is
     * tls_send -- so a caller that has finished writing and is now only
     * reading (a plain request/response fetch, i.e. the common case) would
     * otherwise leave the tail of its own request stuck in our buffer,
     * waiting for a reply that cannot come. */
    tx_flush(s);

    /* drain buffered plaintext first */
    if (s->appoff < s->applen) {
        int n = s->applen - s->appoff; if (n > max) n = max;
        memcpy(buf, s->app + s->appoff, (size_t)n); s->appoff += n;
        return n;
    }
    s->applen = s->appoff = 0;

    if (s->version == TLS_V12) {
        /* TLS 1.2 names the content type in the cleartext record header rather
         * than in a trailing byte inside the ciphertext, so the loop below does
         * not apply; tls12_read_record hands back the type it read. */
        for (;;) {
            uint8_t ct; int dl;
            int r = tls12_read_record(s, &ct, &dl);
            if (r == 0) return 0;
            if (r < 0) return -1;
            if (ct == REC_ALERT) return -1;              /* close_notify etc */
            if (ct == REC_HANDSHAKE) {
                /* A post-handshake HelloRequest is the server asking to
                 * renegotiate. RFC 5746 lets a client decline by ignoring it,
                 * and renegotiation is exactly the feature we do not want, so
                 * ignoring is the whole policy. NewSessionTicket likewise. */
                continue;
            }
            if (ct != REC_APPDATA) continue;
            if (dl == 0) continue;                       /* legal, and not EOF */
            s->applen = dl; s->appoff = 0;
            int n = dl > max ? max : dl;
            memcpy(buf, s->app, (size_t)n); s->appoff = n;
            return n;
        }
    }

    for (;;) {
        int r = rec_pull(s);
        if (r == 0) return 0;                            /* nothing available yet */
        if (r < 0) return -1;
        const uint8_t *body = s->rxrec + 5;
        int blen = s->reclen;
        if (s->rectype == REC_CCS) { rec_drop(s); continue; }
        if (s->rectype != REC_APPDATA) { rec_drop(s); return -1; }
        if (blen - 16 > (int)sizeof s->app) { rec_drop(s); return -1; }
        uint8_t it;
        int dl = aead_open(&s->cr, body, blen, s->app, &it);
        rec_drop(s);
        if (dl < 0) return -1;
        if (it == REC_ALERT) return -1;                  /* close_notify etc */
        if (it == REC_HANDSHAKE) {
            /* Post-handshake messages. KeyUpdate is fatal: ignoring it would
             * leave us decrypting with keys the peer has already rotated, i.e.
             * silent corruption -- so fail loudly instead. NewSessionTicket is
             * now what resumption is built on and is cached. */
            if (dl >= 1 && s->app[0] == HS_KEY_UPDATE) {
                kprintf("[tls] KeyUpdate not supported -- closing\n");
                return -1;
            }
            take_tickets(s, s->app, dl);
            continue;
        }
        if (it != REC_APPDATA) continue;
        s->applen = dl; s->appoff = 0;
        int n = dl > max ? max : dl;
        memcpy(buf, s->app, (size_t)n); s->appoff = n;
        return n;
    }
}

int tls_version(int id)
{
    struct tls_sess *s = sess_of(id);
    return s ? s->version : 0;
}

int tls_resumed(int id)
{
    struct tls_sess *s = sess_of(id);
    return s ? s->psk_accepted : -1;
}

void tls_tickets_clear(void) { tls_psk_clear_all(); }
int  tls_tickets_count(void) { return tls_psk_count(); }

int tls_pending(int id)
{
    struct tls_sess *s = sess_of(id);
    if (!s || s->state != TS_ESTABLISHED) return 0;
    return s->applen - s->appoff;
}

int tls_alpn(int id, char *out, int max)
{
    struct tls_sess *s = sess_of(id);
    if (!s || max <= 0) return -1;
    int i = 0;
    while (s->alpn_sel[i] && i < max - 1) { out[i] = s->alpn_sel[i]; i++; }
    out[i] = 0;
    return i;
}

void tls_close(int id)
{
    if (id < 0 || id >= TLS_MAX_SESSIONS || !sessions[id].used) return;
    /* A session abandoned mid-handshake (the caller gave up, not tls_fail) still
     * spent the time; close the span rather than dropping the sample. */
    tlsprof_close(&sessions[id].prof);
    /* drop traffic keys, buffered plaintext and the in-use flag in one shot */
    crypto_wipe(&sessions[id], sizeof sessions[id]);
}

/* tls_der_sig_to_rs() moved to x509.c as x509_der_sig_to_rs() -- one shared
 * implementation (declared in x509.h). */
