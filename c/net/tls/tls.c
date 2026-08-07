#include <stdint.h>
#include <stddef.h>
#include "tls.h"
#include "tcp.h"
#include "net.h"
#include "pit.h"
#include "crypto.h"
#include "x509.h"
#include "roots.h"
#include "kprintf.h"
#include "rng.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

/* cipher suites */
#define TLS_AES_128_GCM_SHA256       0x1301
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

/* named groups (RFC 8446 4.2.7) */
#define GRP_P256    0x0017
#define GRP_P384    0x0018
#define GRP_X25519  0x001d

/* handshake message types */
#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_ENCRYPTED_EXT  8
#define HS_CERTIFICATE    11
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20
#define HS_KEY_UPDATE     24
#define HS_MESSAGE_HASH   254             /* synthetic, RFC 8446 4.4.1 */

/* record content types */
#define REC_CCS        20
#define REC_ALERT      21
#define REC_HANDSHAKE  22
#define REC_APPDATA    23

/* extensions */
#define EXT_SNI            0
#define EXT_ALPN           16
#define EXT_SUPPORTED_GRPS 10
#define EXT_SIG_ALGS       13
#define EXT_SUPPORTED_VERS 43
#define EXT_COOKIE         44
#define EXT_KEY_SHARE      51

#define HLEN 32                              /* SHA-256 transcript/HKDF */

/* Largest TLSCiphertext.length RFC 8446 5.2 permits: 2^14 content + 256 for the
 * inner type, padding and the AEAD tag. Anything longer is a protocol error, so
 * the receive buffer only ever needs one such record plus its 5-byte header. */
#define REC_BODY_MAX  16640
#define RXBUF         (5 + REC_BODY_MAX + 16)

/* Outbound application records are capped well below the 2^14 maximum. That is
 * a deliberate memory trade: the send path has to hold a whole sealed record
 * until TCP accepts it (a half-written record is unparseable, so it cannot be
 * abandoned), and 8 sessions x 16 KiB of send buffer is 128 KiB of kernel BSS
 * bought for nothing -- a client's outbound traffic is requests, which are
 * small. Inbound is unaffected and still takes full-size records. */
#define SEND_REC_MAX  4096
#define TXBUF         (SEND_REC_MAX + 512)

/* The server's first encrypted flight: EncryptedExtensions + Certificate +
 * CertificateVerify + Finished. Real chains run 4-6 KiB, including the 4-cert
 * RSA-4096 ones; 16 KiB has been enough for every chain we have met and bounds
 * what a hostile server can make us buffer. */
#define HSBUF         16384

struct aead {
    int suite;                               /* cipher suite id */
    uint8_t key[32]; int keylen;             /* 32 chacha / 16 aes */
    uint8_t iv[12];
    uint64_t seq;
};

/* Handshake-phase secrets. Kept in the session (not on the stack) because the
 * handshake is now steppable -- it returns to the caller between flights. Wiped
 * the moment the application traffic keys are derived. */
struct hs_secrets {
    uint8_t hs[32];                          /* handshake secret */
    uint8_t c_hs[32], s_hs[32];              /* client/server handshake traffic */
};

enum {
    TS_FREE = 0,
    TS_SEND_CH,        /* build + queue ClientHello (first or post-HRR) */
    TS_RECV_SH,        /* read ServerHello, or a HelloRetryRequest */
    TS_RECV_FLIGHT,    /* read the encrypted EE..Finished flight */
    TS_FIN_FLUSH,      /* our Finished is queued; drain it */
    TS_ESTABLISHED,
    TS_FAILED,
};

struct tls_sess {
    int used, tcp, state, err;
    int64_t now;                             /* cert-validity clock, captured at start */
    uint64_t deadline;                       /* absolute tick budget for the handshake */

    char host[256];
    char alpn_offer[96];                     /* comma-separated list, as given */
    char alpn_sel[32];                       /* what the server picked ("" = none) */

    /* ClientHello fields that must be *identical* in CH1 and CH2 (RFC 8446
     * 4.1.2): only the key_share and cookie may change across a retry. */
    uint8_t random[32], sid[32];

    int group;                               /* GRP_* we have a private key for */
    uint8_t priv[48];                        /* x25519 scalar (32) or EC scalar (32/48) */
    uint8_t pub[97];                         /* our share, on the wire */
    int publen;
    int hrr_seen;                            /* exactly one HelloRetryRequest allowed */
    int ccs_sent;                            /* the one compat CCS (RFC 8446 D.4) */
    uint8_t cookie[512]; int cookielen;      /* echoed back in CH2 if HRR sent one */

    int suite;
    struct sha256 th;                        /* running handshake transcript */
    struct hs_secrets sec;
    struct aead cr, cw;                      /* read (server) / write (client) keys */

    /* transport buffers */
    uint8_t rxrec[RXBUF]; int rxlen;         /* raw bytes / one partial record */
    int reclen; uint8_t rectype;             /* the record rec_pull() exposed */
    uint8_t tx[TXBUF]; int txlen, txoff;     /* sealed bytes waiting for TCP */
    uint8_t hsbuf[HSBUF]; int hslen;         /* server handshake flight */
    uint8_t app[REC_BODY_MAX]; int applen, appoff;   /* decrypted app data */
};

static struct tls_sess sessions[TLS_MAX_SESSIONS];

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
static int fail(struct tls_sess *s, int rc)
{
    crypto_wipe(&s->sec, sizeof s->sec);
    crypto_wipe(&s->cr, sizeof s->cr);
    crypto_wipe(&s->cw, sizeof s->cw);
    crypto_wipe(s->priv, sizeof s->priv);
    s->state = TS_FAILED;
    s->err = rc;
    return rc;
}

/* ------------------------------------------------------------------ record
 * I/O. Both halves are strictly non-blocking: they move whatever TCP will give
 * or take right now and report "would block" otherwise. Nothing in this file
 * calls net_poll() -- that is the caller's job, which is what makes the whole
 * handshake steppable. */

/* Push queued bytes at TCP. 1 = everything drained, 0 = would block, -1 fatal. */
static int tx_flush(struct tls_sess *s)
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
static int tx_queue(struct tls_sess *s, uint8_t type, const uint8_t *body, int len)
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
static int rec_pull(struct tls_sess *s)
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

static void rec_drop(struct tls_sess *s)
{
    int used = 5 + s->reclen;
    if (used > s->rxlen) used = s->rxlen;
    memmove(s->rxrec, s->rxrec + used, (size_t)(s->rxlen - used));
    s->rxlen -= used;
    s->reclen = 0;
}

/* --- AEAD record seal/open (TLS 1.3) --- */
static void make_nonce(const struct aead *a, uint8_t nonce[12])
{
    memcpy(nonce, a->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(a->seq >> (8 * i));
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
    if (a->suite == TLS_CHACHA20_POLY1305_SHA256)
        chacha20_poly1305_seal(a->key, nonce, aad, 5, plain, plen, out, out + plen);
    else
        aes128_gcm_seal(a->key, nonce, aad, 5, plain, plen, out, out + plen);
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
    int rc;
    if (a->suite == TLS_CHACHA20_POLY1305_SHA256)
        rc = chacha20_poly1305_open(a->key, nonce, aad, 5, body, plen, body + plen, out);
    else
        rc = aes128_gcm_open(a->key, nonce, aad, 5, body, plen, body + plen, out);
    if (rc) return -1;
    a->seq++;
    while (plen > 0 && out[plen - 1] == 0) plen--;       /* strip padding */
    if (plen == 0) return -1;
    *inner_type = out[--plen];                            /* trailing real type */
    return plen;
}

/* --- key schedule helpers --- */
/* Labels are all <=12-char literals and ctx is a 32-byte hash (or NULL), so
 * hkdf_expand_label()'s bounds are always satisfied and it cannot fail here. */
static void derive_secret(const uint8_t *secret, const char *label,
                          const uint8_t *thash, uint8_t *out)
{ hkdf_expand_label(HLEN, secret, label, thash, HLEN, out, HLEN); }

static void traffic_keys(const uint8_t *secret, int suite, struct aead *a)
{
    a->suite = suite;
    a->keylen = (suite == TLS_CHACHA20_POLY1305_SHA256) ? 32 : 16;
    hkdf_expand_label(HLEN, secret, "key", 0, 0, a->key, a->keylen);
    hkdf_expand_label(HLEN, secret, "iv", 0, 0, a->iv, 12);
    a->seq = 0;
}

static void transcript_hash(const struct sha256 *running, uint8_t out[32])
{ struct sha256 c = *running; sha256_final(&c, out); }

/* big-endian helpers for building messages */
static int put_u16(uint8_t *p, int v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; return 2; }

/* ------------------------------------------------------------------- groups */

static int group_curve(int grp) { return grp == GRP_P256 ? 256 : grp == GRP_P384 ? 384 : 0; }
static const char *group_name(int grp)
{
    return grp == GRP_X25519 ? "x25519" : grp == GRP_P256 ? "secp256r1"
         : grp == GRP_P384 ? "secp384r1" : "?";
}
static int group_supported(int grp)
{ return grp == GRP_X25519 || grp == GRP_P256 || grp == GRP_P384; }

/* Generate a fresh ephemeral private key + public share for s->group.
 * The EC path retries on an out-of-range scalar rather than reducing one: a
 * reduction would bias the private key toward small values. Eight tries makes
 * the failure probability (about 2^-32 per try for P-256) unreachable. */
static int gen_share(struct tls_sess *s)
{
    if (s->group == GRP_X25519) {
        rand_bytes(s->priv, 32);
        x25519_base(s->pub, s->priv);
        s->publen = 32;
        return 0;
    }
    int curve = group_curve(s->group);
    if (!curve) return -1;
    int flen = curve / 8;
    for (int try = 0; try < 8; try++) {
        rand_bytes(s->priv, flen);
        if (ecdh_keygen(curve, s->priv, rand_u32(), s->pub) == 0) {
            s->publen = 1 + 2 * flen;
            return 0;
        }
    }
    return -1;
}

/* Derive the ECDHE shared secret from the server's key share. */
static int compute_shared(struct tls_sess *s, const uint8_t *spub, int splen,
                          uint8_t *out, int *outlen)
{
    if (s->group == GRP_X25519) {
        if (splen != 32) return -1;
        x25519(out, s->priv, spub);
        /* RFC 7748/8446 contributory check: an all-zero shared secret means the
         * peer sent a low-order point, so the "secret" is one the peer chose. */
        uint8_t z = 0; for (int i = 0; i < 32; i++) z |= out[i];
        if (!z) return -1;
        *outlen = 32;
        return 0;
    }
    int curve = group_curve(s->group);
    if (!curve) return -1;
    /* ecdh_shared range- and on-curve-checks the peer point; with cofactor 1
     * that rules out invalid-curve and small-subgroup attacks outright. */
    if (ecdh_shared(curve, s->priv, rand_u32(), spub, splen, out) != 0) return -1;
    *outlen = curve / 8;
    return 0;
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
static int build_ch(struct tls_sess *s, uint8_t *ch, int max)
{
    int hl = 0; while (s->host[hl]) hl++;
    int n = 0;
    /* Everything up to the ALPN extension is fixed-size apart from the host
     * name, so one check here covers all of it; the two variable-length
     * extensions after it (ALPN, cookie, key_share) check themselves. */
    if (max < 160 + hl) return -1;

    ch[n++] = HS_CLIENT_HELLO; int lenpos = n; n += 3;   /* 3-byte length, filled later */
    ch[n++] = 0x03; ch[n++] = 0x03;                      /* legacy_version */
    memcpy(ch + n, s->random, 32); n += 32;
    ch[n++] = 32; memcpy(ch + n, s->sid, 32); n += 32;   /* legacy_session_id */
    n += put_u16(ch + n, 4);
    n += put_u16(ch + n, TLS_AES_128_GCM_SHA256);
    n += put_u16(ch + n, TLS_CHACHA20_POLY1305_SHA256);  /* cipher_suites */
    ch[n++] = 1; ch[n++] = 0;                            /* compression: null */
    int extlenpos = n; n += 2;

    /* server_name (SNI) */
    n += put_u16(ch + n, EXT_SNI); n += put_u16(ch + n, hl + 5);
    n += put_u16(ch + n, hl + 3); ch[n++] = 0; n += put_u16(ch + n, hl);
    memcpy(ch + n, s->host, (size_t)hl); n += hl;

    /* supported_versions = TLS 1.3 */
    n += put_u16(ch + n, EXT_SUPPORTED_VERS); n += put_u16(ch + n, 3);
    ch[n++] = 2; n += put_u16(ch + n, 0x0304);

    /* supported_groups. x25519 is listed first because it is the group we have
     * a constant-time implementation for; the two NIST curves are there so a
     * server that refuses x25519 gets a HelloRetryRequest it can act on
     * instead of a handshake_failure. */
    n += put_u16(ch + n, EXT_SUPPORTED_GRPS); n += put_u16(ch + n, 8);
    n += put_u16(ch + n, 6);
    n += put_u16(ch + n, GRP_X25519);
    n += put_u16(ch + n, GRP_P256);
    n += put_u16(ch + n, GRP_P384);

    /* signature_algorithms */
    n += put_u16(ch + n, EXT_SIG_ALGS); n += put_u16(ch + n, 16); n += put_u16(ch + n, 14);
    n += put_u16(ch + n, 0x0403); n += put_u16(ch + n, 0x0503);
    n += put_u16(ch + n, 0x0804); n += put_u16(ch + n, 0x0805); n += put_u16(ch + n, 0x0806);
    n += put_u16(ch + n, 0x0401); n += put_u16(ch + n, 0x0501);

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

    /* key_share: exactly one entry, for the group we hold a key for. Offering
     * only x25519 up front (rather than pre-generating a NIST share too) keeps
     * the common handshake cheap -- a P-256 keygen on this bignum is far more
     * expensive than the extra round trip an HRR costs, and the servers that
     * need it are rare. */
    if (n + 12 + s->publen > max) return -1;
    n += put_u16(ch + n, EXT_KEY_SHARE); n += put_u16(ch + n, s->publen + 6);
    n += put_u16(ch + n, s->publen + 4);
    n += put_u16(ch + n, s->group); n += put_u16(ch + n, s->publen);
    memcpy(ch + n, s->pub, (size_t)s->publen); n += s->publen;

    put_u16(ch + extlenpos, n - extlenpos - 2);
    ch[lenpos] = 0;
    ch[lenpos+1] = (uint8_t)((n - lenpos - 3) >> 8);
    ch[lenpos+2] = (uint8_t)(n - lenpos - 3);
    return n;
}

/* -------------------------------------------------------------- steps */

static int step_send_ch(struct tls_sess *s)
{
    uint8_t ch[1600];
    int n = build_ch(s, ch, (int)sizeof ch);
    if (n < 0) return fail(s, TLS_E_PROTO);

    /* The single dummy CCS of RFC 8446 D.4 goes immediately before our second
     * flight -- which is ClientHello2 when there was a retry, and the Finished
     * otherwise. Exactly one, either way. */
    if (s->hrr_seen && !s->ccs_sent) {
        uint8_t ccs = 1;
        if (tx_queue(s, REC_CCS, &ccs, 1)) return fail(s, TLS_E_PROTO);
        s->ccs_sent = 1;
    }
    sha256_update(&s->th, ch, (size_t)n);
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
    uint8_t ch1[32];
    transcript_hash(&s->th, ch1);
    sha256_init(&s->th);
    uint8_t hdr[4] = { HS_MESSAGE_HASH, 0, 0, HLEN };
    sha256_update(&s->th, hdr, 4);
    sha256_update(&s->th, ch1, HLEN);
}

/* Parse a ServerHello / HelloRetryRequest body. Fills *suite and, for a real
 * ServerHello, the server key share; for an HRR, *retry_group / cookie.
 * Every read is bounded by shend, which is bounded by what we received. */
static int parse_sh(struct tls_sess *s, const uint8_t *body, int blen, int shend,
                    int *suite, const uint8_t **spub, int *splen, int *retry_group)
{
    (void)blen;
    int p = 4 + 2 + 32;                                  /* skip type/len, ver, random */
    int sidlen = body[p++]; p += sidlen;
    if (p + 5 > shend) return -1;                        /* suite(2)+comp(1)+extlen(2) */
    *suite = (body[p] << 8) | body[p+1]; p += 2;
    p += 1;                                              /* compression */
    int elen = (body[p] << 8) | body[p+1]; p += 2;
    int eend = p + elen;
    if (eend > shend) return -1;
    int saw_version = 0;
    while (p + 4 <= eend) {
        int et = (body[p] << 8) | body[p+1], el = (body[p+2] << 8) | body[p+3]; p += 4;
        if (p + el > eend) return -1;
        if (et == EXT_SUPPORTED_VERS && el == 2) {
            if (((body[p] << 8) | body[p+1]) != 0x0304) return -1;
            saw_version = 1;
        } else if (et == EXT_KEY_SHARE) {
            if (retry_group) {                           /* HRR: group only, no key */
                if (el != 2) return -1;
                *retry_group = (body[p] << 8) | body[p+1];
            } else {
                if (el < 4) return -1;
                int grp = (body[p] << 8) | body[p+1];
                int kl = (body[p+2] << 8) | body[p+3];
                if (grp != s->group || kl != el - 4) return -1;   /* must answer our offer */
                *spub = body + p + 4; *splen = kl;
            }
        } else if (et == EXT_COOKIE && retry_group) {
            if (el < 2) return -1;
            int cl = (body[p] << 8) | body[p+1];
            if (cl != el - 2 || cl > (int)sizeof s->cookie) return -1;
            memcpy(s->cookie, body + p + 2, (size_t)cl);
            s->cookielen = cl;
        }
        p += el;
    }
    /* A TLS 1.3 server MUST send supported_versions in its ServerHello; without
     * it we would be talking to something that negotiated an older version in
     * the legacy field, which we do not implement. */
    if (!saw_version) return -1;
    return 0;
}

/* Report a plaintext alert -- these carry the reason a server rejected us
 * (e.g. 40 handshake_failure when no group/suite overlapped), and printing it
 * is the difference between a debuggable failure and "TLS_E_PROTO". */
static void log_alert(const uint8_t *body, int blen)
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

    if (!is_hrr) {
        /* Downgrade protection (RFC 8446 4.1.3): a genuine TLS 1.3 server sets
         * the last 8 bytes of ServerHello.random to a fixed sentinel iff it
         * negotiated a *lower* version. We only speak 1.3, so either sentinel
         * means someone forced a downgrade -- abort. */
        static const uint8_t d12[8] = {0x44,0x4F,0x57,0x4E,0x47,0x52,0x44,0x01};
        static const uint8_t d11[8] = {0x44,0x4F,0x57,0x4E,0x47,0x52,0x44,0x00};
        if (memcmp(body + 6 + 24, d12, 8) == 0 || memcmp(body + 6 + 24, d11, 8) == 0)
            return fail(s, TLS_E_PROTO);
    }

    if (is_hrr) {
        if (s->hrr_seen) {                      /* RFC 8446 4.1.4: at most one */
            kprintf("[tls] second HelloRetryRequest -- aborting\n");
            return fail(s, TLS_E_PROTO);
        }
        int suite = 0, grp = -1;
        transcript_restart_for_hrr(s);
        sha256_update(&s->th, body, (size_t)shend);
        if (parse_sh(s, body, blen, shend, &suite, 0, 0, &grp) != 0)
            return fail(s, TLS_E_PROTO);
        rec_drop(s);
        /* The retry must ask for a group we offered in supported_groups and
         * did NOT already send a share for; otherwise the server is either
         * confused or trying to make us loop. */
        if (!group_supported(grp) || grp == s->group) {
            kprintf("[tls] HRR asked for group 0x%x (unusable) -- aborting\n", grp);
            return fail(s, TLS_E_PROTO);
        }
        if (suite != TLS_AES_128_GCM_SHA256 && suite != TLS_CHACHA20_POLY1305_SHA256)
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
    int suite = 0, splen = 0;
    const uint8_t *spub = 0;
    sha256_update(&s->th, body, (size_t)shend);
    if (parse_sh(s, body, blen, shend, &suite, &spub, &splen, 0) != 0)
        return fail(s, TLS_E_PROTO);
    if (!spub || splen < 1) return fail(s, TLS_E_PROTO);
    if (suite != TLS_CHACHA20_POLY1305_SHA256 && suite != TLS_AES_128_GCM_SHA256)
        return fail(s, TLS_E_PROTO);
    s->suite = suite;

    /* Copy the peer's share out before rec_drop() compacts the receive buffer:
     * spub points INTO s->rxrec, and dropping the record memmoves everything
     * behind it down over exactly those bytes. Reading it afterwards is not a
     * crash -- it silently agrees on the wrong secret, which then surfaces
     * hundreds of lines later as a Finished MAC mismatch. */
    uint8_t speer[97];
    if (splen > (int)sizeof speer) return fail(s, TLS_E_PROTO);
    memcpy(speer, spub, (size_t)splen);
    rec_drop(s);

    /* --- key schedule: handshake secrets --- */
    uint8_t shared[48]; int sharedlen = 0;
    if (compute_shared(s, speer, splen, shared, &sharedlen) != 0) {
        crypto_wipe(shared, sizeof shared);
        return fail(s, TLS_E_CRYPTO);
    }
    uint8_t zeros[32]; memset(zeros, 0, 32);
    uint8_t early[32], derived[32], emptyhash[32];
    hkdf_extract(HLEN, 0, 0, zeros, 32, early);
    sha256("", 0, emptyhash);
    derive_secret(early, "derived", emptyhash, derived);
    hkdf_extract(HLEN, derived, HLEN, shared, sharedlen, s->sec.hs);
    uint8_t th_chsh[32]; transcript_hash(&s->th, th_chsh);
    derive_secret(s->sec.hs, "s hs traffic", th_chsh, s->sec.s_hs);
    derive_secret(s->sec.hs, "c hs traffic", th_chsh, s->sec.c_hs);
    traffic_keys(s->sec.s_hs, suite, &s->cr);
    traffic_keys(s->sec.c_hs, suite, &s->cw);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(early, sizeof early);
    crypto_wipe(derived, sizeof derived);

    kprintf("[tls] ServerHello: suite 0x%x, group %s%s\n",
            suite, group_name(s->group), s->hrr_seen ? " (after HRR)" : "");
    s->state = TS_RECV_FLIGHT;
    /* Servers send the whole flight back-to-back, so EncryptedExtensions is
     * usually already in the buffer. Carry straight on instead of reporting
     * WANT_READ for data we are holding. */
    return step_recv_flight(s);
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
    /* Hash through Certificate. Zeroed so that a CertificateVerify arriving
     * before any Certificate fails verification rather than reading garbage. */
    uint8_t th_cert[32] = { 0 };
    const uint8_t *flight = s->hsbuf;
    int flen = s->hslen;
    int q = 0;

    while (q + 4 <= flen) {
        int mt = flight[q];
        int ml = (flight[q+1] << 16) | (flight[q+2] << 8) | flight[q+3];
        if (q + 4 + ml > flen) break;                    /* ignore an incomplete tail */
        const uint8_t *mb = flight + q + 4;

        if (mt == HS_ENCRYPTED_EXT) {
            sha256_update(&s->th, flight + q, (size_t)(4 + ml));
            /* EncryptedExtensions is where the server reports its ALPN choice. */
            int p = 0;
            if (ml >= 2) {
                int elen = (mb[0] << 8) | mb[1]; p = 2;
                int eend = p + elen;
                if (eend <= ml) {
                    while (p + 4 <= eend) {
                        int et = (mb[p] << 8) | mb[p+1], el = (mb[p+2] << 8) | mb[p+3]; p += 4;
                        if (p + el > eend) break;
                        if (et == EXT_ALPN && el >= 3) {
                            int nl = mb[p + 2];
                            if (nl > 0 && nl + 3 <= el && nl < (int)sizeof s->alpn_sel) {
                                memcpy(s->alpn_sel, mb + p + 3, (size_t)nl);
                                s->alpn_sel[nl] = 0;
                            }
                        }
                        p += el;
                    }
                }
            }
        } else if (mt == HS_CERTIFICATE) {
            sha256_update(&s->th, flight + q, (size_t)(4 + ml));
            /* cert_request_ctx(1) + cert_list(3) of {cert(3) + exts(2)}. Every
             * length is bounded by ml, so a crafted Certificate cannot read
             * past the handshake message. */
            if (ml < 4) return TLS_E_PROTO;
            int cp = 1 + mb[0];
            if (cp + 3 > ml) return TLS_E_PROTO;
            int listlen = (mb[cp] << 16) | (mb[cp+1] << 8) | mb[cp+2]; cp += 3;
            int cend = cp + listlen;
            if (cend > ml) return TLS_E_PROTO;
            while (cp + 3 <= cend && ncert < 8) {
                int clen = (mb[cp] << 16) | (mb[cp+1] << 8) | mb[cp+2]; cp += 3;
                if (cp + clen + 2 > cend) return TLS_E_PROTO;
                if (x509_parse(mb + cp, clen, &chain[ncert]) == 0) ncert++;
                cp += clen;
                int extl = (mb[cp] << 8) | mb[cp+1]; cp += 2;
                if (cp + extl > cend) return TLS_E_PROTO;
                cp += extl;
            }
            transcript_hash(&s->th, th_cert);
        } else if (mt == HS_CERT_VERIFY) {
            /* signature over 64*0x20 || "TLS 1.3, server CertificateVerify" || 0 || th_cert */
            if (ml < 4) return TLS_E_PROTO;
            int sigalg = (mb[0] << 8) | mb[1];
            int siglen = (mb[2] << 8) | mb[3];
            if (4 + siglen > ml) return TLS_E_PROTO;
            const uint8_t *sig = mb + 4;
            uint8_t signed_data[64 + 33 + 1 + 32]; int sd = 0;
            for (int i = 0; i < 64; i++) signed_data[sd++] = 0x20;
            const char *ctx = "TLS 1.3, server CertificateVerify";
            for (int i = 0; ctx[i]; i++) signed_data[sd++] = (uint8_t)ctx[i];
            signed_data[sd++] = 0;
            memcpy(signed_data + sd, th_cert, 32); sd += 32;
            int okv = 0;
            if (ncert > 0 && (sigalg == 0x0403 || sigalg == 0x0503)) {
                int curve = (sigalg == 0x0403) ? 256 : 384;
                int flen2 = curve / 8;
                uint8_t hash[48]; int hh;
                if (curve == 256) { sha256(signed_data, sd, hash); hh = 32; }
                else { sha384(signed_data, sd, hash); hh = 48; }
                uint8_t rs[96];
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
            if (!okv) {
                kprintf("[tls] CertificateVerify rejected (sigalg 0x%x, %d certs)\n", sigalg, ncert);
                return TLS_E_CERT;
            }
            sha256_update(&s->th, flight + q, (size_t)(4 + ml));
        } else if (mt == HS_FINISHED) {
            uint8_t th_cv[32]; transcript_hash(&s->th, th_cv);   /* through CertVerify */
            uint8_t fk[32], expect[32];
            hkdf_expand_label(HLEN, s->sec.s_hs, "finished", 0, 0, fk, 32);
            hmac(HLEN, fk, 32, th_cv, 32, expect);
            int bad = (ml != 32) || memcmp(expect, mb, 32) != 0;
            crypto_wipe(fk, sizeof fk); crypto_wipe(expect, sizeof expect);
            if (bad) return TLS_E_CRYPTO;
            sha256_update(&s->th, flight + q, (size_t)(4 + ml));
        }
        q += 4 + ml;
    }

    /* Say WHY a chain was refused. "TLS_E_CERT" is indistinguishable between an
     * expired leaf, a name mismatch and an anchor we do not hold, and those
     * three call for completely different responses from whoever is looking. */
    if (ncert < 1) { kprintf("[tls] no usable certificate in the flight\n"); return TLS_E_CERT; }
    int vr = x509_verify_chain(chain, ncert, s->host, s->now);
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
        int dl = aead_open(&s->cr, body, blen, s->app, &it);
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
    uint8_t th_full[32]; transcript_hash(&s->th, th_full);
    uint8_t cfk[32], cverify[32];
    hkdf_expand_label(HLEN, s->sec.c_hs, "finished", 0, 0, cfk, 32);
    hmac(HLEN, cfk, 32, th_full, 32, cverify);
    uint8_t fin[4 + 32];
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    memcpy(fin + 4, cverify, 32);
    crypto_wipe(cfk, sizeof cfk); crypto_wipe(cverify, sizeof cverify);

    if (!s->ccs_sent) {                                  /* RFC 8446 D.4, see step_send_ch */
        uint8_t ccs = 1;
        if (tx_queue(s, REC_CCS, &ccs, 1)) return fail(s, TLS_E_PROTO);
        s->ccs_sent = 1;
    }
    uint8_t finrec[64];
    int frl = aead_seal(&s->cw, REC_HANDSHAKE, fin, (int)sizeof fin, finrec);
    if (frl < 0 || tx_queue(s, REC_APPDATA, finrec, frl)) return fail(s, TLS_E_PROTO);

    /* --- application traffic secrets --- */
    uint8_t emptyhash[32], derived2[32], master[32], c_ap[32], s_ap[32], zeros[32];
    memset(zeros, 0, 32);
    sha256("", 0, emptyhash);
    derive_secret(s->sec.hs, "derived", emptyhash, derived2);
    hkdf_extract(HLEN, derived2, HLEN, zeros, 32, master);
    derive_secret(master, "c ap traffic", th_full, c_ap);
    derive_secret(master, "s ap traffic", th_full, s_ap);
    traffic_keys(c_ap, s->suite, &s->cw);
    traffic_keys(s_ap, s->suite, &s->cr);
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
    return TLS_DONE;
}

/* ------------------------------------------------------------- public API */

/* Say out loud, once, how big the trust store actually is -- including the
 * roots that were dropped at generation time because we cannot verify their key
 * type. tools/roots/ holding 130 PEMs means nothing if some of them never made
 * it into the binary; this is the line that stops that from being invisible. */
static void trust_banner(void)
{
    static int done;
    if (done) return;
    done = 1;
    kprintf("[tls] trust store: %d roots, %d skipped\n", logit_nroots, logit_nroots_skipped);
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
    s->group = GRP_X25519;
    if (gen_share(s) != 0) { fail(s, TLS_E_CRYPTO); return TLS_E_CRYPTO; }
    s->state = TS_SEND_CH;
    sha256_init(&s->th);
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
    case TS_SEND_CH:     return step_send_ch(s);
    case TS_RECV_SH:     return step_recv_sh(s);
    case TS_RECV_FLIGHT: return step_recv_flight(s);
    case TS_FIN_FLUSH:   s->state = TS_ESTABLISHED; return TLS_DONE;
    default:             return fail(s, TLS_E_PROTO);
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
         * has to deliver the very bytes we are waiting for. */
        net_poll();
        net_idle();
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
    uint8_t rec[SEND_REC_MAX + 32];
    int rl = aead_seal(&s->cw, REC_APPDATA, buf, n, rec);
    if (rl < 0 || tx_queue(s, REC_APPDATA, rec, rl)) return -1;
    if (tx_flush(s) < 0) return -1;
    /* The record is sealed and owned by the session now; whether TCP took all
     * of it is irrelevant to the caller, who must not resend these bytes. */
    return n;
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
            /* Post-handshake messages. NewSessionTicket is safe to drop (we do
             * not do resumption yet). KeyUpdate is NOT: ignoring it would leave
             * us decrypting with keys the peer has already rotated, i.e. silent
             * corruption -- so fail loudly instead. */
            if (dl >= 1 && s->app[0] == HS_KEY_UPDATE) {
                kprintf("[tls] KeyUpdate not supported -- closing\n");
                return -1;
            }
            continue;
        }
        if (it != REC_APPDATA) continue;
        s->applen = dl; s->appoff = 0;
        int n = dl > max ? max : dl;
        memcpy(buf, s->app, (size_t)n); s->appoff = n;
        return n;
    }
}

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
    /* drop traffic keys, buffered plaintext and the in-use flag in one shot */
    crypto_wipe(&sessions[id], sizeof sessions[id]);
}

/* tls_der_sig_to_rs() moved to x509.c as x509_der_sig_to_rs() -- one shared
 * implementation (declared in x509.h). */
