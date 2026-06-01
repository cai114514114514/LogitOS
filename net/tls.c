#include <stdint.h>
#include <stddef.h>
#include "tls.h"
#include "tcp.h"
#include "net.h"
#include "pit.h"
#include "crypto.h"
#include "x509.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

/* cipher suites */
#define TLS_AES_128_GCM_SHA256       0x1301
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

/* handshake message types */
#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_ENCRYPTED_EXT  8
#define HS_CERTIFICATE    11
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20

/* record content types */
#define REC_CCS        20
#define REC_ALERT      21
#define REC_HANDSHAKE  22
#define REC_APPDATA    23

#define HLEN 32                              /* SHA-256 transcript/HKDF */

struct aead {
    int suite;                               /* cipher suite id */
    uint8_t key[32]; int keylen;             /* 32 chacha / 16 aes */
    uint8_t iv[12];
    uint64_t seq;
};

struct tls_sess {
    int used, tcp;
    struct aead cr, cw;                      /* read (server) / write (client) keys */
    int established;
    /* partial record reassembly from TCP */
    uint8_t rxrec[20000]; int rxlen;
    /* decrypted application data not yet consumed */
    uint8_t app[16384]; int applen, appoff;
};

static struct tls_sess sessions[2];

/* --- weak randomness (client nonce / ephemeral key; not a CSPRNG) --- */
static uint64_t rng_state = 0x123456789abcdefULL;
static void rand_bytes(uint8_t *b, int n)
{
    rng_state ^= timer_ticks() * 0x9e3779b97f4a7c15ULL + 1;
    for (int i = 0; i < n; i++) {
        rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i] = (uint8_t)(rng_state >> 33);
    }
}

/* --- TCP record I/O (blocking with timeout; pumps net_poll) --- */
/* Read exactly one TLS record: hdr[5] + body. Returns body length or -1. */
static int rec_read(struct tls_sess *s, uint8_t *type, uint8_t *body, int maxbody)
{
    uint64_t start = timer_ticks();
    for (;;) {
        if (s->rxlen >= 5) {
            int blen = (s->rxrec[3] << 8) | s->rxrec[4];
            if (blen > maxbody || blen > (int)sizeof s->rxrec - 5) return -1;
            if (s->rxlen >= 5 + blen) {
                *type = s->rxrec[0];
                memcpy(body, s->rxrec + 5, blen);
                int consumed = 5 + blen;
                memmove(s->rxrec, s->rxrec + consumed, (size_t)(s->rxlen - consumed));
                s->rxlen -= consumed;
                return blen;
            }
        }
        if (timer_ticks() - start > 800) return -1;      /* ~8s */
        net_poll();
        int n = tcp_recv(s->tcp, s->rxrec + s->rxlen, (int)sizeof s->rxrec - s->rxlen);
        if (n > 0) { s->rxlen += n; start = timer_ticks(); }
        else if (n < 0) return -1;
        else for (volatile int d = 0; d < 100000; d++) ;
    }
}

static int rec_write(struct tls_sess *s, uint8_t type, const uint8_t *body, int len)
{
    uint8_t hdr[5] = { type, 0x03, 0x03, (uint8_t)(len >> 8), (uint8_t)len };
    if (tcp_send(s->tcp, hdr, 5) < 0) return -1;
    if (len && tcp_send(s->tcp, body, len) < 0) return -1;
    return 0;
}

/* --- AEAD record seal/open (TLS 1.3) --- */
static void make_nonce(const struct aead *a, uint8_t nonce[12])
{
    memcpy(nonce, a->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(a->seq >> (8 * i));
}

/* Encrypt inner (content||inner_type) into a record body; returns body len. */
static int aead_seal(struct aead *a, uint8_t inner_type, const uint8_t *content, int clen,
                     uint8_t *out)
{
    uint8_t plain[16384];
    memcpy(plain, content, clen); plain[clen] = inner_type;
    int plen = clen + 1;
    int rlen = plen + 16;
    uint8_t aad[5] = { REC_APPDATA, 0x03, 0x03, (uint8_t)(rlen >> 8), (uint8_t)rlen };
    uint8_t nonce[12]; make_nonce(a, nonce);
    if (a->suite == TLS_CHACHA20_POLY1305_SHA256)
        chacha20_poly1305_seal(a->key, nonce, aad, 5, plain, plen, out, out + plen);
    else
        aes128_gcm_seal(a->key, nonce, aad, 5, plain, plen, out, out + plen);
    a->seq++;
    return rlen;
}

/* Decrypt a record body in place; returns plaintext len and *inner_type, or -1. */
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

int tls_connect(int tcp_id, const char *host, int64_t now)
{
    int id = -1;
    for (int i = 0; i < 2; i++) if (!sessions[i].used) { id = i; break; }
    if (id < 0) return TLS_E_PROTO;
    struct tls_sess *s = &sessions[id];
    memset(s, 0, sizeof *s);
    s->used = 1; s->tcp = tcp_id;

    /* our X25519 ephemeral keypair */
    uint8_t priv[32], pub[32];
    rand_bytes(priv, 32);
    x25519_base(pub, priv);

    int hl = 0; while (host[hl]) hl++;

    /* --- build ClientHello --- */
    uint8_t ch[512]; int n = 0;
    ch[n++] = HS_CLIENT_HELLO; int lenpos = n; n += 3;   /* 3-byte length filled later */
    ch[n++] = 0x03; ch[n++] = 0x03;                      /* legacy_version */
    rand_bytes(ch + n, 32); n += 32;                     /* random */
    ch[n++] = 32; rand_bytes(ch + n, 32); n += 32;       /* legacy_session_id */
    n += put_u16(ch + n, 4); n += put_u16(ch + n, TLS_AES_128_GCM_SHA256);
    n += put_u16(ch + n, TLS_CHACHA20_POLY1305_SHA256);  /* cipher_suites */
    ch[n++] = 1; ch[n++] = 0;                            /* compression: null */
    int extlenpos = n; n += 2;                           /* extensions length */
    /* server_name (SNI) */
    n += put_u16(ch + n, 0); n += put_u16(ch + n, hl + 5);
    n += put_u16(ch + n, hl + 3); ch[n++] = 0; n += put_u16(ch + n, hl);
    memcpy(ch + n, host, hl); n += hl;
    /* supported_versions = TLS 1.3 */
    n += put_u16(ch + n, 43); n += put_u16(ch + n, 3); ch[n++] = 2; n += put_u16(ch + n, 0x0304);
    /* supported_groups = x25519 (0x001d) */
    n += put_u16(ch + n, 10); n += put_u16(ch + n, 4); n += put_u16(ch + n, 2); n += put_u16(ch + n, 0x001d);
    /* signature_algorithms */
    n += put_u16(ch + n, 13); n += put_u16(ch + n, 12); n += put_u16(ch + n, 10);
    n += put_u16(ch + n, 0x0403); n += put_u16(ch + n, 0x0503);
    n += put_u16(ch + n, 0x0804); n += put_u16(ch + n, 0x0805); n += put_u16(ch + n, 0x0401);
    /* key_share: x25519 + our pubkey */
    n += put_u16(ch + n, 51); n += put_u16(ch + n, 38); n += put_u16(ch + n, 36);
    n += put_u16(ch + n, 0x001d); n += put_u16(ch + n, 32); memcpy(ch + n, pub, 32); n += 32;
    put_u16(ch + extlenpos, n - extlenpos - 2);
    ch[lenpos] = 0; ch[lenpos+1] = (uint8_t)((n - lenpos - 3) >> 8); ch[lenpos+2] = (uint8_t)(n - lenpos - 3);

    struct sha256 th; sha256_init(&th);
    sha256_update(&th, ch, n);
    if (rec_write(s, REC_HANDSHAKE, ch, n)) { s->used=0; return TLS_E_TCP; }

    /* --- read ServerHello --- */
    static uint8_t body[20000]; uint8_t rtype;
    int blen = rec_read(s, &rtype, body, sizeof body);
    if (blen < 0 || rtype != REC_HANDSHAKE || body[0] != HS_SERVER_HELLO) { s->used=0; return TLS_E_PROTO; }
    int shlen = (body[1]<<16)|(body[2]<<8)|body[3];
    sha256_update(&th, body, 4 + shlen);
    /* parse SH: cipher suite + server key share */
    int p = 4 + 2 + 32;                                  /* skip ver+random */
    int sidlen = body[p++]; p += sidlen;                 /* session id echo */
    int suite = (body[p]<<8)|body[p+1]; p += 2;
    p += 1;                                              /* compression */
    int elen = (body[p]<<8)|body[p+1]; p += 2;
    int eend = p + elen; const uint8_t *spub = 0;
    while (p + 4 <= eend) {
        int et = (body[p]<<8)|body[p+1], el = (body[p+2]<<8)|body[p+3]; p += 4;
        if (et == 51 && el >= 4) {                       /* key_share */
            int grp = (body[p]<<8)|body[p+1]; int kl = (body[p+2]<<8)|body[p+3];
            if (grp == 0x001d && kl == 32) spub = body + p + 4;
        }
        p += el;
    }
    if (!spub || (suite != TLS_CHACHA20_POLY1305_SHA256 && suite != TLS_AES_128_GCM_SHA256)) { s->used=0; return TLS_E_PROTO; }

    /* --- key schedule: handshake secrets --- */
    uint8_t shared[32]; x25519(shared, priv, spub);
    uint8_t zeros[32]; memset(zeros, 0, 32);
    uint8_t early[32], derived[32], hs[32];
    hkdf_extract(HLEN, 0, 0, zeros, 32, early);
    uint8_t emptyhash[32]; sha256(zeros, 0, emptyhash);  /* hash of "" */
    sha256("", 0, emptyhash);
    derive_secret(early, "derived", emptyhash, derived);
    hkdf_extract(HLEN, derived, HLEN, shared, 32, hs);
    uint8_t th_chsh[32]; transcript_hash(&th, th_chsh);
    uint8_t s_hs[32], c_hs[32];
    derive_secret(hs, "s hs traffic", th_chsh, s_hs);
    derive_secret(hs, "c hs traffic", th_chsh, c_hs);
    traffic_keys(s_hs, suite, &s->cr);
    traffic_keys(c_hs, suite, &s->cw);

    /* --- read encrypted handshake flight: EE, Certificate, CertVerify, Finished --- */
    static uint8_t flight[16384]; int flen = 0;
    int got_fin = 0;
    while (!got_fin) {
        blen = rec_read(s, &rtype, body, sizeof body);
        if (blen < 0) { s->used=0; return TLS_E_PROTO; }
        if (rtype == REC_CCS) continue;                  /* ignore middlebox CCS */
        if (rtype != REC_APPDATA) { s->used=0; return TLS_E_PROTO; }
        static uint8_t dec[20000]; uint8_t it;
        int dl = aead_open(&s->cr, body, blen, dec, &it);
        if (dl < 0) { s->used=0; return TLS_E_CRYPTO; }
        if (it != REC_HANDSHAKE) continue;
        if (flen + dl > (int)sizeof flight) { s->used=0; return TLS_E_PROTO; }
        memcpy(flight + flen, dec, dl); flen += dl;
        /* do we now hold a complete Finished at the end? scan messages */
        int q = 0;
        while (q + 4 <= flen) {
            int mt = flight[q]; int ml = (flight[q+1]<<16)|(flight[q+2]<<8)|flight[q+3];
            if (q + 4 + ml > flen) break;
            if (mt == HS_FINISHED) got_fin = 1;
            q += 4 + ml;
        }
    }

    /* --- walk the flight: feed transcript, verify cert + certverify + finished --- */
    static struct cert chain[8]; int ncert = 0;
    uint8_t th_cert[32];                                 /* hash thru Certificate */
    int q = 0;
    while (q + 4 <= flen) {
        int mt = flight[q]; int ml = (flight[q+1]<<16)|(flight[q+2]<<8)|flight[q+3];
        const uint8_t *mb = flight + q + 4;
        if (mt == HS_ENCRYPTED_EXT) {
            sha256_update(&th, flight + q, 4 + ml);
        } else if (mt == HS_CERTIFICATE) {
            sha256_update(&th, flight + q, 4 + ml);
            /* parse: cert_request_ctx(1) + cert_list(3) of {cert(3) + exts(2)} */
            int cp = 0; cp += 1 + mb[0];                 /* skip request context */
            int listlen = (mb[cp]<<16)|(mb[cp+1]<<8)|mb[cp+2]; cp += 3;
            int cend = cp + listlen;
            while (cp + 3 <= cend && ncert < 8) {
                int clen = (mb[cp]<<16)|(mb[cp+1]<<8)|mb[cp+2]; cp += 3;
                if (x509_parse(mb + cp, clen, &chain[ncert]) == 0) ncert++;
                cp += clen;
                int extl = (mb[cp]<<8)|mb[cp+1]; cp += 2 + extl;
            }
            transcript_hash(&th, th_cert);
        } else if (mt == HS_CERT_VERIFY) {
            /* signature over: 64*0x20 || "TLS 1.3, server CertificateVerify" || 0 || th_cert */
            int sigalg = (mb[0]<<8)|mb[1];
            int siglen = (mb[2]<<8)|mb[3];
            const uint8_t *sig = mb + 4;
            uint8_t signed_data[64 + 33 + 1 + 32]; int sd = 0;
            for (int i = 0; i < 64; i++) signed_data[sd++] = 0x20;
            const char *ctx = "TLS 1.3, server CertificateVerify";
            for (int i = 0; ctx[i]; i++) signed_data[sd++] = (uint8_t)ctx[i];
            signed_data[sd++] = 0;
            memcpy(signed_data + sd, th_cert, 32); sd += 32;
            /* verify with leaf public key; supported: ecdsa_secp256r1_sha256(0x0403),
             * ecdsa_secp384r1_sha384(0x0503) */
            int okv = 0;
            if (ncert > 0 && (sigalg == 0x0403 || sigalg == 0x0503)) {
                int curve = (sigalg == 0x0403) ? 256 : 384;
                int flen2 = curve / 8;
                uint8_t hash[48]; int hh;
                if (curve == 256) { sha256(signed_data, sd, hash); hh = 32; }
                else { sha384(signed_data, sd, hash); hh = 48; }
                /* DER sig -> r||s (reuse the conversion via a tiny inline) */
                extern int tls_der_sig_to_rs(const uint8_t*, int, uint8_t*, int);
                uint8_t rs[96];
                if (tls_der_sig_to_rs(sig, siglen, rs, flen2) == 0 &&
                    chain[0].pub[0] == 0x04 &&
                    ecdsa_verify(curve, chain[0].pub + 1, rs, hash, hh)) okv = 1;
            }
            if (!okv) { s->used=0; return TLS_E_CERT; }
            sha256_update(&th, flight + q, 4 + ml);
        } else if (mt == HS_FINISHED) {
            uint8_t th_cv[32]; transcript_hash(&th, th_cv);   /* thru CertVerify */
            uint8_t fk[32], expect[32];
            hkdf_expand_label(HLEN, s_hs, "finished", 0, 0, fk, 32);
            hmac(HLEN, fk, 32, th_cv, 32, expect);
            if (ml != 32 || memcmp(expect, mb, 32) != 0) { s->used=0; return TLS_E_CRYPTO; }
            sha256_update(&th, flight + q, 4 + ml);
        }
        q += 4 + ml;
    }

    /* verify the certificate chain (strict) */
    if (ncert < 1 || x509_verify_chain(chain, ncert, host, now) != X509_OK) { s->used=0; return TLS_E_CERT; }

    /* --- client Finished --- */
    uint8_t th_full[32]; transcript_hash(&th, th_full);
    uint8_t cfk[32], cverify[32];
    hkdf_expand_label(HLEN, c_hs, "finished", 0, 0, cfk, 32);
    hmac(HLEN, cfk, 32, th_full, 32, cverify);
    uint8_t fin[4 + 32]; fin[0]=HS_FINISHED; fin[1]=0; fin[2]=0; fin[3]=32; memcpy(fin+4, cverify, 32);
    uint8_t finrec[64];
    int frl = aead_seal(&s->cw, REC_HANDSHAKE, fin, sizeof fin, finrec);
    /* client also sends a dummy CCS first (compat) */
    uint8_t ccs = 1; rec_write(s, REC_CCS, &ccs, 1);
    if (rec_write(s, REC_APPDATA, finrec, frl)) { s->used=0; return TLS_E_TCP; }

    /* --- application traffic secrets --- */
    uint8_t derived2[32], master[32];
    derive_secret(hs, "derived", emptyhash, derived2);
    hkdf_extract(HLEN, derived2, HLEN, zeros, 32, master);
    uint8_t c_ap[32], s_ap[32];
    derive_secret(master, "c ap traffic", th_full, c_ap);
    derive_secret(master, "s ap traffic", th_full, s_ap);
    traffic_keys(c_ap, suite, &s->cw);
    traffic_keys(s_ap, suite, &s->cr);

    s->established = 1;
    return id;
}

int tls_send(int id, const void *buf, int len)
{
    if (id < 0 || id >= 2 || !sessions[id].used) return -1;
    struct tls_sess *s = &sessions[id];
    static uint8_t rec[16400];
    int rl = aead_seal(&s->cw, REC_APPDATA, buf, len, rec);
    return rec_write(s, REC_APPDATA, rec, rl) ? -1 : len;
}

int tls_recv(int id, void *buf, int max)
{
    if (id < 0 || id >= 2 || !sessions[id].used) return -1;
    struct tls_sess *s = &sessions[id];
    /* drain buffered plaintext first */
    if (s->appoff < s->applen) {
        int n = s->applen - s->appoff; if (n > max) n = max;
        memcpy(buf, s->app + s->appoff, n); s->appoff += n;
        return n;
    }
    s->applen = s->appoff = 0;
    uint8_t rtype; static uint8_t body[20000];
    int blen = rec_read(s, &rtype, body, sizeof body);
    if (blen < 0) return -1;
    if (rtype == REC_CCS) return 0;
    if (rtype != REC_APPDATA) return -1;
    uint8_t it; int dl = aead_open(&s->cr, body, blen, s->app, &it);
    if (dl < 0) return -1;
    if (it == REC_ALERT) return -1;                       /* close_notify etc */
    if (it != REC_APPDATA) return 0;                      /* skip post-handshake msgs */
    s->applen = dl; s->appoff = 0;
    int n = dl > max ? max : dl;
    memcpy(buf, s->app, n); s->appoff = n;
    return n;
}

void tls_close(int id)
{
    if (id < 0 || id >= 2 || !sessions[id].used) return;
    sessions[id].used = 0;
}

/* DER ECDSA sig SEQ{r,s} -> fixed-width r||s (shared with x509.c logic). */
int tls_der_sig_to_rs(const uint8_t *sig, int len, uint8_t *rs, int flen)
{
    const uint8_t *p = sig, *end = sig + len;
    if (p >= end || *p++ != 0x30) return -1;
    int sl = *p++; if (sl & 0x80) { int nb = sl & 0x7f; sl = 0; while (nb--) sl = (sl<<8)|*p++; }
    for (int half = 0; half < 2; half++) {
        if (p >= end || *p++ != 0x02) return -1;
        int il = *p++;
        const uint8_t *ic = p; p += il;
        while (il > 0 && ic[0] == 0) { ic++; il--; }
        if (il > flen) return -1;
        uint8_t *dst = rs + half * flen;
        for (int i = 0; i < flen; i++) dst[i] = 0;
        for (int i = 0; i < il; i++) dst[flen - il + i] = ic[i];
    }
    return 0;
}
