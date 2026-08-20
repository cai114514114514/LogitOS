/* TLS 1.3 server (RFC 8446). See tls_server.h for what it does and does not do.
 *
 * ===========================================================================
 * SHARING WITH tls.c -- READ THIS BEFORE ADDING ANYTHING HERE
 * ===========================================================================
 * Everything this file could reach through tls_int.h, it uses: the session
 * struct, the record I/O (tls_rec_pull / tls_rec_drop / tls_tx_queue /
 * tls_tx_flush), the two-hash transcript (tls_th_update / tls_th_hash), the
 * AEAD dispatch (tls_aead_encrypt / tls_aead_decrypt), the ephemeral key
 * handling (tls_gen_share / tls_compute_shared / tls_group_supported) and the
 * terminal-failure path (tls_fail). None of that is duplicated.
 *
 * FIVE FUNCTIONS ARE DUPLICATED, and they are named here rather than quietly
 * copied, because a second key schedule is exactly how two key schedules come
 * to disagree -- and the disagreement does not show up where it is made. It
 * shows up as a Finished MAC mismatch several hundred lines later, in the
 * peer, pointing at nothing.
 *
 *     tls.c:207  derive_secret()     one line over hkdf_expand_label
 *     tls.c:214  empty_hash()        picks sha256/sha384 by width
 *     tls.c:217  traffic_keys()      secret -> struct aead (key, iv, alg)
 *     tls.c:171  aead_seal()         content -> TLS 1.3 record body
 *     tls.c:189  aead_open()         record body -> content + inner type
 *
 * All five are `static`. They are not client-shaped -- every one of them is a
 * pure function of a secret, a suite and a buffer, with no ClientHello and no
 * session state in sight. THE ONLY THING BLOCKING REUSE IS THE KEYWORD. What
 * this file needs is for those five to lose `static` and be declared in
 * tls_int.h beside the other shared helpers, at which point the block below
 * deletes and this comment with it. That edit was out of scope for the change
 * that added this file (see the note in the final report), so the duplication
 * is here, deliberately, with three properties that bound its cost:
 *
 *   1. Both copies call the SAME hkdf_expand_label / hkdf_extract / hmac out
 *      of c/crypto. What is duplicated is the ORDER of the derivations, not
 *      the arithmetic, and the order is twelve lines.
 *   2. A disagreement is structurally fatal and cannot be silent: the
 *      handshake ends at a Finished MAC. There is no "works but is weaker"
 *      outcome for this particular duplication -- which is precisely why it
 *      was judged safe to carry temporarily and why, say, a duplicated
 *      certificate check would not have been.
 *   3. `make test-tls-server` runs OUR CLIENT against OUR SERVER as one of its
 *      cases, so the two schedules are diffed against each other on every run
 *      -- and both against openssl, which shares neither.
 * ===========================================================================
 */
#include <stdint.h>
#include <stddef.h>
#include "tls.h"
#include "tls_int.h"
#include "tls_server.h"
#include "tcp.h"
#include "pit.h"
#include "crypto.h"
#include "x509.h"
#include "kprintf.h"
#include "rng.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

/* --------------------------------------------------------------- sessions */

/* Sub-states. Deliberately NOT the client's TS_* values: the two state
 * machines share a struct and nothing else, and reusing TS_RECV_FLIGHT to mean
 * "waiting for the client's Finished" would make a debugger print a lie. */
enum {
    SS_FREE = 0,
    SS_RECV_CH,          /* read (and re-read, after HRR) the ClientHello */
    SS_SEND_FLIGHT,      /* SH + CCS queued; drain the sealed flight */
    SS_RECV_FIN,         /* read the client's Finished */
    SS_ESTABLISHED,
    SS_FAILED,
};

struct srv_sess {
    struct tls_sess s;                  /* everything shared with the client */
    const struct tls_ident *ident;
    int  sub;
    int  used;
    /* The client's legacy_session_id, echoed verbatim in ServerHello (RFC 8446
     * 4.1.3). Not s->sid: that field holds the id a CLIENT generated, and
     * overloading it would make "did we send this or echo it?" unanswerable. */
    uint8_t sid_echo[32]; int sid_echolen;
    char sni[256];
    /* signature_algorithms, as offered. Kept because CertificateVerify has to
     * pick from it -- signing with an algorithm the client did not offer is a
     * handshake the client kills at the signature, blaming the key. */
    uint16_t sigalgs[48]; int nsigalgs;
    int  hrr_sent;                      /* at most one, RFC 8446 4.1.4 */
    int  ccs_sent;
    /* The staged outgoing flight lives in s.hsbuf (16 KiB), which by then has
     * finished its incoming job -- the ClientHello is fully parsed before a
     * single flight byte is built. flight_off is how much of it has been
     * sealed and queued, so a WANT_WRITE can be resumed rather than restarted. */
    int  flight_off;
    uint8_t s_ap[48], c_ap[48];         /* application secrets, until both
                                         * directions have switched */
};

static struct srv_sess ssn[TLSS_MAX_SESSIONS];
static int srv_prof_slot = -1;

static struct srv_sess *sess_of(int id)
{
    if (id < 0 || id >= TLSS_MAX_SESSIONS || !ssn[id].used) return 0;
    return &ssn[id];
}

static void rand_bytes(uint8_t *b, int n) { kernel_random_bytes(b, n); }
static uint32_t rand_u32(void) { uint32_t v; rand_bytes((uint8_t *)&v, 4); return v; }

/* --------------------------------------------------------------- alerts --
 * A server that just closes the socket makes every failure look identical to
 * the peer, and "openssl s_client hung up" is not a diagnosis. Best effort by
 * construction: if the transport will not take the alert we do not wait for
 * it, because the session is already lost. */
static int seal_record(struct srv_sess *v, uint8_t inner, const uint8_t *p, int n,
                       uint8_t *out);

/* RFC 8446 6: in TLS 1.3 the level field is vestigial -- every alert is fatal
 * except close_notify and user_canceled -- but it is still on the wire and
 * still read. close_notify MUST go out as warning(1); sending it as fatal(2)
 * turns an orderly shutdown into an error in the peer's log, which is a lie
 * that costs somebody an afternoon. */
static void srv_alert_lv(struct srv_sess *v, uint8_t level, uint8_t desc)
{
    uint8_t body[2] = { level, desc };
    if (v->s.cw.keylen) {                       /* keys are live: it must be sealed */
        uint8_t rec[64];
        int rl = seal_record(v, REC_ALERT, body, 2, rec);
        if (rl > 0) tls_tx_queue(&v->s, REC_APPDATA, rec, rl);
    } else {
        tls_tx_queue(&v->s, REC_ALERT, body, 2);
    }
    tls_tx_flush(&v->s);
}

static void srv_alert(struct srv_sess *v, uint8_t desc) { srv_alert_lv(v, 2, desc); }

static int sfail(struct srv_sess *v, uint8_t alert, int rc)
{
    srv_alert(v, alert);
    v->sub = SS_FAILED;
    return tls_fail(&v->s, rc);
}

/* alert descriptions actually used (RFC 8446 6.2) */
#define AL_HANDSHAKE_FAILURE   40
#define AL_ILLEGAL_PARAMETER   47
#define AL_DECODE_ERROR        50
#define AL_DECRYPT_ERROR       51
#define AL_PROTOCOL_VERSION    70
#define AL_INTERNAL_ERROR      80
#define AL_MISSING_EXTENSION   109
#define AL_CLOSE_NOTIFY         0

/* ================= DUPLICATED FROM tls.c -- see the header ================ */

/* The hash width the KEY SCHEDULE runs at. Normally the suite's, via
 * s->hashlen -- this indirection exists only for the negative control below. */
#ifdef LOGIT_TLSS_BREAK_HASH32
/* NEGATIVE CONTROL (test-tls-server-negctl). Pin the schedule at SHA-256 width
 * regardless of the suite: the single most likely way to get
 * TLS_AES_256_GCM_SHA384 wrong on a server, and invisible to every AES-128 and
 * ChaCha20 case because for those 32 IS the answer. It must redden EXACTLY the
 * one SHA-384 case; if the suite goes green with this defined, the SHA-384 row
 * is not being run and two of the three suites we claim are unmeasured. */
#define SCHED_HL(s) 32
#else
#define SCHED_HL(s) ((s)->hashlen)
#endif


static void derive_secret(int hl, const uint8_t *secret, const char *label,
                          const uint8_t *thash, uint8_t *out)
{ hkdf_expand_label(hl, secret, label, thash, hl, out, hl); }

static void empty_hash(int hl, uint8_t *out)
{ if (hl == 48) sha384("", 0, out); else sha256("", 0, out); }

static void traffic_keys(const uint8_t *secret, int suite, struct aead *a)
{
    int hl = tls13_suite_hash(suite);
    a->alg = (suite == TLS_CHACHA20_POLY1305_SHA256) ? AEAD_CHACHA20
           : (suite == TLS_AES_256_GCM_SHA384)       ? AEAD_AES_256_GCM
                                                     : AEAD_AES_128_GCM;
    a->keylen = (a->alg == AEAD_AES_128_GCM) ? 16 : 32;
    a->ivlen = 12;
    a->explicit_nonce = 0;
    hkdf_expand_label(hl, secret, "key", 0, 0, a->key, a->keylen);
    hkdf_expand_label(hl, secret, "iv", 0, 0, a->iv, 12);
    a->seq = 0;
}

static void make_nonce(const struct aead *a, uint8_t nonce[12])
{
    memcpy(nonce, a->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(a->seq >> (8 * i));
}

/* content -> record body. `out` needs n + 1 + 16 bytes. */
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

static int aead_open(struct aead *a, const uint8_t *body, int blen,
                     uint8_t *out, uint8_t *inner_type)
{
    if (blen < 17) return -1;
    int plen = blen - 16;
    uint8_t aad[5] = { REC_APPDATA, 0x03, 0x03, (uint8_t)(blen >> 8), (uint8_t)blen };
    uint8_t nonce[12]; make_nonce(a, nonce);
    if (tls_aead_decrypt(a, nonce, aad, 5, body, plen, body + plen, out)) return -1;
    a->seq++;
    while (plen > 0 && out[plen - 1] == 0) plen--;
    if (plen == 0) return -1;
    *inner_type = out[--plen];
    return plen;
}

/* ====================== end of the duplicated block ======================= */

static int seal_record(struct srv_sess *v, uint8_t inner, const uint8_t *p, int n,
                       uint8_t *out)
{ return aead_seal(&v->s.cw, inner, p, n, out); }

/* ------------------------------------------------------------ DER writing --
 * Just enough to emit ONE self-signed certificate. Deliberately not a general
 * encoder: x509.c is 536 lines of PARSER because parsing is where the
 * adversary is, and a general writer would be a second surface with no second
 * adversary. Everything here writes a fixed shape with a handful of variable
 * fields, and the whole of it is checked by handing the result to openssl. */

struct db { uint8_t *p; int n, max, err; };

static void db_raw(struct db *d, const void *src, int n)
{
    if (d->err || d->n + n > d->max) { d->err = 1; return; }
    memcpy(d->p + d->n, src, (size_t)n); d->n += n;
}
static void db_u8(struct db *d, uint8_t b) { db_raw(d, &b, 1); }

/* Open a TLV with a ONE-byte length placeholder; db_close shifts the body if
 * the real length needs more. DER demands the minimal encoding and openssl
 * enforces it, so reserving three bytes and back-filling -- the obvious
 * alternative -- produces a certificate openssl refuses to parse. */
static int db_open(struct db *d, uint8_t tag) { db_u8(d, tag); db_u8(d, 0); return d->n; }

static void db_close(struct db *d, int body)
{
    if (d->err) return;
    int len = d->n - body;
    if (len < 0x80) { d->p[body - 1] = (uint8_t)len; return; }
    int extra = (len < 0x100) ? 1 : 2;
    if (len > 0xffff || d->n + extra > d->max) { d->err = 1; return; }
    memmove(d->p + body + extra, d->p + body, (size_t)len);
    d->p[body - 1] = (uint8_t)(0x80 | extra);
    if (extra == 1) d->p[body] = (uint8_t)len;
    else { d->p[body] = (uint8_t)(len >> 8); d->p[body + 1] = (uint8_t)len; }
    d->n += extra;
}

static void db_tlv(struct db *d, uint8_t tag, const void *v, int n)
{ int b = db_open(d, tag); db_raw(d, v, n); db_close(d, b); }

static const uint8_t OID_ECPK[]   = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};        /* id-ecPublicKey */
static const uint8_t OID_P256_[]  = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const uint8_t OID_P384_[]  = {0x2b,0x81,0x04,0x00,0x22};
static const uint8_t OID_P521_[]  = {0x2b,0x81,0x04,0x00,0x23};
static const uint8_t OID_ECDSA256[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_ECDSA384[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const uint8_t OID_ECDSA512[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04};
static const uint8_t OID_CN_[]    = {0x55,0x04,0x03};
static const uint8_t OID_BC_[]    = {0x55,0x1d,0x13};
static const uint8_t OID_KU_[]    = {0x55,0x1d,0x0f};
static const uint8_t OID_SAN_[]   = {0x55,0x1d,0x11};
static const uint8_t OID_EKU_[]   = {0x55,0x1d,0x25};
static const uint8_t OID_SERVERAUTH[] = {0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01};

static const uint8_t *curve_oid(int curve, int *len)
{
    if (curve == 384) { *len = sizeof OID_P384_; return OID_P384_; }
    if (curve == 521) { *len = sizeof OID_P521_; return OID_P521_; }
    *len = sizeof OID_P256_; return OID_P256_;
}
static const uint8_t *sigalg_oid(int curve, int *len)
{
    if (curve == 384) { *len = sizeof OID_ECDSA384; return OID_ECDSA384; }
    if (curve == 521) { *len = sizeof OID_ECDSA512; return OID_ECDSA512; }
    *len = sizeof OID_ECDSA256; return OID_ECDSA256;
}
static int curve_hlen(int curve) { return curve == 384 ? 48 : curve == 521 ? 64 : 32; }
static int curve_flen(int curve) { return curve == 384 ? 48 : curve == 521 ? 66 : 32; }
/* The TLS 1.3 SignatureScheme for an EC key of this curve. Each code point
 * pins BOTH the curve and the hash (RFC 8446 4.2.3), which is why there is no
 * separate hash choice anywhere in this file. */
static int curve_sigalg(int curve) { return curve == 384 ? 0x0503 : curve == 521 ? 0x0603 : 0x0403; }

/* raw r||s -> DER SEQUENCE { INTEGER r, INTEGER s }, which is what TLS 1.3
 * carries for ECDSA. The inverse of x509.c's x509_der_sig_to_rs. The leading
 * 0x00 when the high bit is set is not optional: DER INTEGERs are signed, and
 * omitting it makes r negative, which openssl rejects outright. */
static int rs_to_der(const uint8_t *rs, int flen, uint8_t *out, int max)
{
    struct db d = { out, 0, max, 0 };
    int seq = db_open(&d, 0x30);
    for (int half = 0; half < 2; half++) {
        const uint8_t *v = rs + half * flen;
        int off = 0; while (off < flen - 1 && v[off] == 0) off++;   /* strip leading zeros */
        int b = db_open(&d, 0x02);
        if (v[off] & 0x80) db_u8(&d, 0x00);
        db_raw(&d, v + off, flen - off);
        db_close(&d, b);
    }
    db_close(&d, seq);
    return d.err ? -1 : d.n;
}

/* --------------------------------------------------------------- time -----
 * unix seconds -> "YYMMDDHHMMSSZ" (UTCTime) or "YYYYMMDDHHMMSSZ"
 * (GeneralizedTime). RFC 5280 4.1.2.5 requires UTCTime through 2049 and
 * GeneralizedTime from 2050, and openssl enforces it -- a GeneralizedTime
 * notBefore in 2026 is a certificate real verifiers reject. */
static void civil(int64_t days, int *y, int *m, int *dd)
{
    /* Howard Hinnant's civil_from_days: shift the epoch to 0000-03-01 so leap
     * days land at the end of the year and the month arithmetic is exact. */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t yy  = yoe + era * 400;
    int64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    int64_t mp  = (5*doy + 2)/153;
    *dd = (int)(doy - (153*mp+2)/5 + 1);
    *m  = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y  = (int)(yy + (*m <= 2));
}

static int put_time(struct db *d, int64_t t)
{
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    int y, m, dd; civil(days, &y, &m, &dd);
    int hh = (int)(secs / 3600), mi = (int)(secs / 60 % 60), ss = (int)(secs % 60);
    char b[16]; int n = 0;
    #define D2(v) do { b[n++] = (char)('0' + (v)/10 % 10); b[n++] = (char)('0' + (v)%10); } while (0)
    if (y < 2050) { D2(y % 100); }
    else          { D2(y / 100); D2(y % 100); }
    D2(m); D2(dd); D2(hh); D2(mi); D2(ss);
    #undef D2
    b[n++] = 'Z';
    db_tlv(d, y < 2050 ? 0x17 : 0x18, b, n);
    return 0;
}

static void put_alg(struct db *d, int curve)
{
    int ol; const uint8_t *o = sigalg_oid(curve, &ol);
    int a = db_open(d, 0x30);
    db_tlv(d, 0x06, o, ol);
    /* No parameters. RFC 5758 3.2: for ecdsa-with-SHA*, the parameters field
     * MUST be absent -- an explicit NULL here is what a writer copied from the
     * RSA shape produces, and openssl rejects it. */
    db_close(d, a);
}

/* Name ::= SEQUENCE { RDN SET { AttributeTypeAndValue { CN, UTF8String } } } */
static void put_name(struct db *d, const char *cn, int cnlen)
{
    int n = db_open(d, 0x30);
    int set = db_open(d, 0x31);
    int atv = db_open(d, 0x30);
    db_tlv(d, 0x06, OID_CN_, sizeof OID_CN_);
    db_tlv(d, 0x0c, cn, cnlen);
    db_close(d, atv); db_close(d, set); db_close(d, n);
}

static void put_ext(struct db *d, const uint8_t *oid, int oidlen, int critical,
                    const uint8_t *val, int vallen)
{
    int e = db_open(d, 0x30);
    db_tlv(d, 0x06, oid, oidlen);
    if (critical) { uint8_t t = 0xff; db_tlv(d, 0x01, &t, 1); }
    db_tlv(d, 0x04, val, vallen);
    db_close(d, e);
}

int tlss_self_signed(struct tls_ident *id, const char *cn, int64_t now, int days,
                     uint8_t *buf, int buflen)
{
    if (!id || !cn || !buf) return -1;
    int cnlen = 0; while (cn[cnlen]) cnlen++;
    if (cnlen < 1 || cnlen > 200) return -1;
    /* Refuse to mint a key from the weak fallback RNG, the same refusal
     * tls_start makes and for a stronger reason: an ephemeral ECDHE scalar
     * from a bad source loses one session, a certificate key from a bad source
     * loses the identity for as long as the certificate lives. */
    if (!rng_strong()) {
        kprintf("[tlss] refusing to generate a key: weak RNG (no rdrand/rdseed)\n");
        return -1;
    }

    const int curve = 256;                  /* see the note in tls_server.h */
    const int flen = curve_flen(curve), hlen = curve_hlen(curve);
    uint8_t priv[66], pub[133];
    int okkey = 0;
    /* Retry rather than reduce, exactly as tls_gen_share does: reducing a
     * random string mod n biases the scalar toward small values. */
    for (int t = 0; t < 8 && !okkey; t++) {
        rand_bytes(priv, flen);
        if (ecdh_keygen(curve, priv, rand_u32(), pub) == 0) okkey = 1;
    }
    if (!okkey) { crypto_wipe(priv, sizeof priv); return -1; }

    /* --- TBSCertificate --- */
    struct db d = { buf, 0, buflen, 0 };
    int cert = db_open(&d, 0x30);
    int tbs_at = d.n;
    int tbs = db_open(&d, 0x30);
    {   /* [0] EXPLICIT version = 2 (v3). v3 is not cosmetic: extensions --
         * and therefore subjectAltName -- exist only in v3, and a v1
         * certificate with no SAN fails x509.c's name check on the CN
         * fallback path only, which is a different code path from the one
         * every real certificate uses. */
        int v = db_open(&d, 0xa0); db_tlv(&d, 0x02, "\x02", 1); db_close(&d, v);
    }
    {   /* serialNumber: 8 random bytes forced positive and nonzero. Random
         * rather than a counter because a counter needs storage that survives
         * a reboot, which this machine does not give a certificate. */
        uint8_t ser[9]; rand_bytes(ser + 1, 8);
        ser[1] &= 0x7f; if (!ser[1]) ser[1] = 1;
        db_tlv(&d, 0x02, ser + 1, 8);
    }
    put_alg(&d, curve);
    put_name(&d, cn, cnlen);                       /* issuer == subject */
    { int va = db_open(&d, 0x30);
      put_time(&d, now - 3600);                    /* an hour of clock slack */
      put_time(&d, now + (int64_t)days * 86400);
      db_close(&d, va); }
    put_name(&d, cn, cnlen);
    {   /* SubjectPublicKeyInfo */
        int spki = db_open(&d, 0x30);
        int alg = db_open(&d, 0x30);
        db_tlv(&d, 0x06, OID_ECPK, sizeof OID_ECPK);
        int col; const uint8_t *co = curve_oid(curve, &col);
        db_tlv(&d, 0x06, co, col);
        db_close(&d, alg);
        int bs = db_open(&d, 0x03);
        db_u8(&d, 0x00);                           /* unused bits */
        db_raw(&d, pub, 1 + 2*flen);
        db_close(&d, bs);
        db_close(&d, spki);
    }
    {   /* [3] EXPLICIT extensions */
        int x = db_open(&d, 0xa3);
        int xs = db_open(&d, 0x30);
        /* basicConstraints CA:TRUE, critical.
         *
         * A serving certificate that is also a CA looks wrong and is not: this
         * certificate IS ITS OWN TRUST ANCHOR. A verifier reaches it by
         * finding it in a trust store, and both openssl's verifier and
         * x509.c's is_pinned_root/signed_by_root treat the top of a chain as a
         * root -- which openssl additionally requires to be a CA. CA:FALSE
         * here yields "unable to get local issuer certificate" from
         * s_client with the very certificate it is verifying sitting in
         * -CAfile, which is a confusing way to fail. This is exactly what
         * `openssl req -x509` emits by default, for the same reason. */
        { uint8_t bc[8]; struct db b = { bc, 0, sizeof bc, 0 };
          int q = db_open(&b, 0x30); db_tlv(&b, 0x01, "\xff", 1); db_close(&b, q);
          put_ext(&d, OID_BC_, sizeof OID_BC_, 1, bc, b.n); }
        /* keyUsage: digitalSignature (bit 0) + keyCertSign (bit 5), critical.
         * digitalSignature is what CertificateVerify needs; keyCertSign is
         * what a verifier demands of the anchor it just used to check a
         * self-signature. Both, because this certificate does both jobs. */
        { uint8_t ku[4] = { 0x03, 0x02, 0x01, 0x84 };   /* BIT STRING, 1 unused bit, 10000100 */
          put_ext(&d, OID_KU_, sizeof OID_KU_, 1, ku, 4); }
        /* subjectAltName: dNSName = cn. x509.c prefers SAN and FAILS CLOSED if
         * a SAN is present and does not match, so the CN is not a fallback
         * here -- this extension is the name check. */
        { uint8_t san[256]; struct db b = { san, 0, sizeof san, 0 };
          int q = db_open(&b, 0x30); db_tlv(&b, 0x82, cn, cnlen); db_close(&b, q);
          if (b.err) { crypto_wipe(priv, sizeof priv); return -1; }
          put_ext(&d, OID_SAN_, sizeof OID_SAN_, 0, san, b.n); }
        /* extKeyUsage: serverAuth. */
        { uint8_t eku[32]; struct db b = { eku, 0, sizeof eku, 0 };
          int q = db_open(&b, 0x30); db_tlv(&b, 0x06, OID_SERVERAUTH, sizeof OID_SERVERAUTH);
          db_close(&b, q);
          put_ext(&d, OID_EKU_, sizeof OID_EKU_, 0, eku, b.n); }
        db_close(&d, xs); db_close(&d, x);
    }
    db_close(&d, tbs);
    if (d.err) { crypto_wipe(priv, sizeof priv); return -1; }
    int tbslen = d.n - tbs_at;

    /* --- sign the TBS --- */
    uint8_t h[64];
    if (hlen == 32) sha256(buf + tbs_at, (size_t)tbslen, h);
    else if (hlen == 48) sha384(buf + tbs_at, (size_t)tbslen, h);
    else sha512(buf + tbs_at, (size_t)tbslen, h);
    uint8_t rs[132], sigder[160];
    if (ecdsa_sign(curve, priv, h, hlen, rand_u32(), hmac, rs) != 0) {
        crypto_wipe(priv, sizeof priv); return -1;
    }
    int sdl = rs_to_der(rs, flen, sigder, sizeof sigder);
    if (sdl < 0) { crypto_wipe(priv, sizeof priv); return -1; }
    put_alg(&d, curve);                            /* MUST equal the inner one */
    { int bs = db_open(&d, 0x03); db_u8(&d, 0x00); db_raw(&d, sigder, sdl); db_close(&d, bs); }
    db_close(&d, cert);
    if (d.err) { crypto_wipe(priv, sizeof priv); return -1; }

    memset(id, 0, sizeof *id);
    id->chain[0] = buf; id->chainlen[0] = d.n; id->nchain = 1;
    id->key_curve = curve;
    memcpy(id->key, priv, (size_t)flen);
    crypto_wipe(priv, sizeof priv);
    kprintf("[tlss] self-signed P-%d certificate for %s, %d bytes, valid %d days\n",
            curve, cn, d.n, days);
    return d.n;
}

/* ---------------------------------------------------------- ClientHello --- */

static int put_u16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 2; }
static int rd_u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

/* Our suite preference, most preferred first. ONE list: the client's offer
 * order in tls.c leads with AES-128 for the same reason, and a server that
 * ranked them differently would make "which suite did we negotiate" depend on
 * which end of this tree you asked. */
static const uint16_t srv_suites[3] = {
    TLS_AES_128_GCM_SHA256,
    TLS_CHACHA20_POLY1305_SHA256,
    TLS_AES_256_GCM_SHA384,
};
/* Our group preference. x25519 first because it is the constant-time one. */
static const uint16_t srv_groups[3] = { GRP_X25519, GRP_P256, GRP_P384 };

struct ch_info {
    const uint8_t *sid; int sidlen;
    const uint8_t *suites; int suiteslen;
    const uint8_t *groups; int groupslen;        /* supported_groups body */
    const uint8_t *ks; int kslen;                /* key_share client_shares body */
    const uint8_t *sigalgs; int sigalgslen;
    const uint8_t *alpn; int alpnlen;
    const uint8_t *sni; int snilen;
    int  v13;                                    /* supported_versions named 0x0304 */
    int  psk_offered;
};

static int parse_ch(const uint8_t *b, int len, struct ch_info *o)
{
    memset(o, 0, sizeof *o);
    int p = 0;
    if (len < 35) return -1;
    p += 2;                                      /* legacy_version */
    p += 32;                                     /* random */
    if (p >= len) return -1;
    int sl = b[p++];
    if (sl > 32 || p + sl > len) return -1;
    o->sid = b + p; o->sidlen = sl; p += sl;
    if (p + 2 > len) return -1;
    int cs = rd_u16(b + p); p += 2;
    if (cs < 2 || (cs & 1) || p + cs > len) return -1;
    o->suites = b + p; o->suiteslen = cs; p += cs;
    if (p >= len) return -1;
    int cm = b[p++];
    if (p + cm > len) return -1;
    p += cm;
    /* Extensions are OPTIONAL in the ClientHello grammar but a TLS 1.3 hello
     * cannot exist without them; a hello that stops here is a 1.2-or-earlier
     * client and is refused by the caller on !v13, not here. */
    if (p + 2 > len) return 0;
    int el = rd_u16(b + p); p += 2;
    if (p + el > len) return -1;
    int end = p + el;
    while (p + 4 <= end) {
        int et = rd_u16(b + p), ev = rd_u16(b + p + 2); p += 4;
        if (p + ev > end) return -1;
        const uint8_t *v = b + p;
        switch (et) {
        case EXT_SUPPORTED_VERS:
            if (ev >= 1) {
                int n = v[0];
                if (1 + n <= ev) for (int i = 0; i + 1 < n; i += 2)
                    if (rd_u16(v + 1 + i) == TLS_V13) o->v13 = 1;
            }
            break;
        case EXT_SUPPORTED_GRPS:
            if (ev >= 2) { int n = rd_u16(v); if (2 + n <= ev) { o->groups = v + 2; o->groupslen = n; } }
            break;
        case EXT_KEY_SHARE:
            if (ev >= 2) { int n = rd_u16(v); if (2 + n <= ev) { o->ks = v + 2; o->kslen = n; } }
            break;
        case EXT_SIG_ALGS:
            if (ev >= 2) { int n = rd_u16(v); if (2 + n <= ev) { o->sigalgs = v + 2; o->sigalgslen = n; } }
            break;
        case EXT_ALPN:
            if (ev >= 2) { int n = rd_u16(v); if (2 + n <= ev) { o->alpn = v + 2; o->alpnlen = n; } }
            break;
        case EXT_SNI:
            /* ServerNameList of NameType(1) + opaque<0..2^16-1>. Only
             * host_name (0) exists; anything else is skipped rather than
             * refused, because a future name type is not an attack. */
            if (ev >= 5) {
                int n = rd_u16(v);
                if (2 + n <= ev && v[2] == 0) {
                    int hl = rd_u16(v + 3);
                    if (5 + hl <= ev) { o->sni = v + 5; o->snilen = hl; }
                }
            }
            break;
        case EXT_PSK:
            o->psk_offered = 1;
            break;
        default: break;
        }
        p += ev;
    }
    return 0;
}

/* ------------------------------------------------------ handshake output -- */

/* Append a handshake message header + body and feed both to the transcript. */
static int hs_append(struct db *d, struct tls_sess *s, uint8_t type,
                     const uint8_t *body, int blen)
{
    if (d->err || d->n + 4 + blen > d->max) { d->err = 1; return -1; }
    uint8_t *at = d->p + d->n;
    at[0] = type;
    at[1] = (uint8_t)(blen >> 16); at[2] = (uint8_t)(blen >> 8); at[3] = (uint8_t)blen;
    memcpy(at + 4, body, (size_t)blen);
    d->n += 4 + blen;
    tls_th_update(s, at, 4 + blen);
    return 0;
}

/* The HelloRetryRequest random is not a constant typed in from the RFC: RFC
 * 8446 4.1.3 DEFINES it as SHA-256("HelloRetryRequest"), so it is computed.
 * A transcription error in a 32-byte magic value produces a retry the client
 * reads as an ordinary ServerHello and then fails to make sense of. */
static void hrr_random(uint8_t out[32]) { sha256("HelloRetryRequest", 17, out); }

/* ServerHello / HelloRetryRequest share a structure (RFC 8446 4.1.4). */
static int build_sh(struct srv_sess *v, uint8_t *out, int max, int retry)
{
    struct tls_sess *s = &v->s;
    /* 512 covers a ServerHello with the largest classical share we offer (a
     * P-384 point, 97 bytes). It is deliberately NOT sized from
     * TLS_KX_PUB_MAX: that bound is set by the post-quantum hybrid, whose
     * server share is 1120 bytes, and this server does not offer the hybrid
     * (see srv_groups). Adding it means changing both, together. */
    uint8_t body[512]; int n = 0;
    n += put_u16(body + n, 0x0303);               /* legacy_version */
    if (retry) hrr_random(body + n);
    else       memcpy(body + n, s->random, 32);
    n += 32;
    body[n++] = (uint8_t)v->sid_echolen;
    memcpy(body + n, v->sid_echo, (size_t)v->sid_echolen); n += v->sid_echolen;
    n += put_u16(body + n, s->suite);
    body[n++] = 0;                                /* legacy_compression_method */

    int extlen_at = n; n += 2;
    n += put_u16(body + n, EXT_SUPPORTED_VERS); n += put_u16(body + n, 2);
    n += put_u16(body + n, TLS_V13);
    if (retry) {
        /* key_share in an HRR carries ONLY the group we are asking for. */
        n += put_u16(body + n, EXT_KEY_SHARE); n += put_u16(body + n, 2);
        n += put_u16(body + n, s->group);
    } else {
        n += put_u16(body + n, EXT_KEY_SHARE); n += put_u16(body + n, s->publen + 4);
        n += put_u16(body + n, s->group); n += put_u16(body + n, s->publen);
        memcpy(body + n, s->pub, (size_t)s->publen); n += s->publen;
    }
    put_u16(body + extlen_at, n - extlen_at - 2);

    struct db o = { out, 0, max, 0 };
    if (hs_append(&o, s, HS_SERVER_HELLO, body, n) != 0) return -1;
    return o.n;
}

/* RFC 8446 4.4.1: after a HelloRetryRequest the transcript is restarted as
 *   message_hash(254) || 00 00 <hashlen> || Hash(ClientHello1)
 * and CH1 itself is dropped. The 384 context is re-initialised alongside for
 * uniformity only -- once a suite is chosen (which it is, before any HRR is
 * sent) s->hashlen fixes which one is ever read again. */
static void transcript_restart_hrr(struct tls_sess *s)
{
    uint8_t ch1[48]; tls_th_hash(s, ch1);
    int hl = s->hashlen;
    sha256_init(&s->th); sha384_init(&s->th384);
    uint8_t hdr[4] = { HS_MESSAGE_HASH, 0, 0, (uint8_t)hl };
    tls_th_update(s, hdr, 4);
    tls_th_update(s, ch1, hl);
}

static int select_suite(const struct ch_info *ci)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j + 1 < ci->suiteslen; j += 2)
            if (rd_u16(ci->suites + j) == srv_suites[i]) return srv_suites[i];
    return 0;
}

/* Does the client's key_share hold a group we support? Returns the group and
 * writes the share and its length through the two out-parameters. */
static int select_keyshare(const struct ch_info *ci, const uint8_t **pub, int *publen)
{
    for (int i = 0; i < 3; i++) {
        int p = 0;
        while (p + 4 <= ci->kslen) {
            int g = rd_u16(ci->ks + p), l = rd_u16(ci->ks + p + 2);
            if (p + 4 + l > ci->kslen) return 0;
            if (g == srv_groups[i]) { *pub = ci->ks + p + 4; *publen = l; return g; }
            p += 4 + l;
        }
    }
    return 0;
}

/* Failing that, a group we support that the client at least SAID it supports.
 * That is the HelloRetryRequest case, and it is what makes a 2026 browser
 * reach this server at all: Chrome and Firefox put X25519MLKEM768 in
 * key_share and x25519 in supported_groups, so the first loop finds nothing
 * and this one finds the way forward. */
static int select_retry_group(const struct ch_info *ci)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j + 1 < ci->groupslen; j += 2)
            if (rd_u16(ci->groups + j) == srv_groups[i]) return srv_groups[i];
    return 0;
}

/* ALPN: OUR preference order over the client's list, which is the RFC 7301
 * server-decides model. Returns 1 if something was selected. */
static int select_alpn(struct srv_sess *v, const struct ch_info *ci)
{
    struct tls_sess *s = &v->s;
    if (!ci->alpn || !s->alpn_offer[0]) return 0;
    const char *q = s->alpn_offer;
    while (*q) {
        const char *start = q;
        while (*q && *q != ',') q++;
        int ol = (int)(q - start);
        int p = 0;
        while (p < ci->alpnlen) {
            int l = ci->alpn[p];
            if (p + 1 + l > ci->alpnlen) break;
            if (l == ol) {
                int same = 1;
                for (int i = 0; i < l; i++) if (ci->alpn[p+1+i] != (uint8_t)start[i]) { same = 0; break; }
                if (same && ol < (int)sizeof s->alpn_sel) {
                    for (int i = 0; i < ol; i++) s->alpn_sel[i] = start[i];
                    s->alpn_sel[ol] = 0;
                    return 1;
                }
            }
            p += 1 + l;
        }
        if (*q == ',') q++;
    }
    return 0;
}

/* --------------------------------------------------------- the flight ----- */

static int build_flight(struct srv_sess *v)
{
    struct tls_sess *s = &v->s;
    const struct tls_ident *id = v->ident;
    const int hl = SCHED_HL(s);
    struct db d = { s->hsbuf, 0, (int)sizeof s->hsbuf, 0 };

    /* --- EncryptedExtensions --- */
    { uint8_t ee[128]; int n = 2, sel = s->alpn_sel[0] ? 1 : 0;
      if (sel) {
          int l = 0; while (s->alpn_sel[l]) l++;
          n += put_u16(ee + n, EXT_ALPN); n += put_u16(ee + n, l + 3);
          n += put_u16(ee + n, l + 1); ee[n++] = (uint8_t)l;
          memcpy(ee + n, s->alpn_sel, (size_t)l); n += l;
      }
      put_u16(ee, n - 2);
      if (hs_append(&d, s, HS_ENCRYPTED_EXT, ee, n) != 0) return -1; }

    /* --- Certificate --- */
    { /* Built straight into the flight buffer rather than staged: a chain is
       * up to TLSS_CHAIN_BYTES and a second copy of it on the kernel stack is
       * not something to spend 12 KiB on. */
      int total = 0;
      for (int i = 0; i < id->nchain; i++) total += 3 + id->chainlen[i] + 2;
      int blen = 1 + 3 + total;
      if (d.n + 4 + blen > d.max) return -1;
      uint8_t *at = d.p + d.n;
      at[0] = HS_CERTIFICATE;
      at[1] = (uint8_t)(blen >> 16); at[2] = (uint8_t)(blen >> 8); at[3] = (uint8_t)blen;
      uint8_t *b = at + 4; int n = 0;
      b[n++] = 0;                                     /* certificate_request_context */
      b[n++] = (uint8_t)(total >> 16); b[n++] = (uint8_t)(total >> 8); b[n++] = (uint8_t)total;
      for (int i = 0; i < id->nchain; i++) {
          int cl = id->chainlen[i];
          b[n++] = (uint8_t)(cl >> 16); b[n++] = (uint8_t)(cl >> 8); b[n++] = (uint8_t)cl;
          memcpy(b + n, id->chain[i], (size_t)cl); n += cl;
          n += put_u16(b + n, 0);                     /* no CertificateEntry extensions */
      }
      d.n += 4 + blen;
      tls_th_update(s, at, 4 + blen); }

    /* --- CertificateVerify --- */
    { uint8_t th_cert[48]; tls_th_hash(s, th_cert);
      /* RFC 8446 4.4.3: 64 spaces, the context string, a zero, the transcript. */
      uint8_t sd[64 + 33 + 1 + 48]; int n = 0;
#ifndef LOGIT_TLSS_BREAK_CV_PREFIX
      for (int i = 0; i < 64; i++) sd[n++] = 0x20;
#endif
      /* NEGATIVE CONTROL (test-tls-server-negctl): LOGIT_TLSS_BREAK_CV_PREFIX
       * drops the 64 leading 0x20 octets. That prefix exists to stop a
       * signature made in one context being replayed in another, and dropping
       * it is the most plausible misreading of 4.4.3 -- the message still has
       * a context string, a separator and a transcript, so it LOOKS complete.
       * Every peer rejects the result at CertificateVerify, which is where the
       * control has to bite: nothing on OUR side notices, because we never
       * verify our own signature. */
      const char *ctx = "TLS 1.3, server CertificateVerify";
      for (int i = 0; ctx[i]; i++) sd[n++] = (uint8_t)ctx[i];
      sd[n++] = 0;
      memcpy(sd + n, th_cert, (size_t)hl); n += hl;

      int curve = id->key_curve;
      int shlen = curve_hlen(curve), flen = curve_flen(curve);
      uint8_t h[64];
      if (shlen == 32) sha256(sd, (size_t)n, h);
      else if (shlen == 48) sha384(sd, (size_t)n, h);
      else sha512(sd, (size_t)n, h);
      uint8_t rs[132], der[160];
      if (ecdsa_sign(curve, id->key, h, shlen, rand_u32(), hmac, rs) != 0) return -1;
      int dl = rs_to_der(rs, flen, der, sizeof der);
      if (dl < 0) return -1;
      uint8_t cv[4 + 160]; int c = 0;
      c += put_u16(cv + c, curve_sigalg(curve));
      c += put_u16(cv + c, dl);
      memcpy(cv + c, der, (size_t)dl); c += dl;
      if (hs_append(&d, s, HS_CERT_VERIFY, cv, c) != 0) return -1; }

    /* --- Finished --- */
    { uint8_t th_cv[48]; tls_th_hash(s, th_cv);
      uint8_t fk[48], fin[48];
      hkdf_expand_label(hl, s->sec.s_hs, "finished", 0, 0, fk, hl);
      hmac(hl, fk, hl, th_cv, hl, fin);
      crypto_wipe(fk, sizeof fk);
      if (hs_append(&d, s, HS_FINISHED, fin, hl) != 0) return -1;
      crypto_wipe(fin, sizeof fin); }

    if (d.err) return -1;
    s->hslen = d.n;
    v->flight_off = 0;
    return 0;
}

/* Application secrets. Derived HERE, right after the flight is built, because
 * they are keyed on the transcript through the SERVER's Finished -- the last
 * message the flight contains. Deriving them after the client's Finished
 * arrives would key them one message too far. */
static void derive_app_secrets(struct srv_sess *v)
{
    struct tls_sess *s = &v->s;
    const int hl = SCHED_HL(s);
    uint8_t emptyhash[48], derived[48], master[48], zeros[48], th[48];
    memset(zeros, 0, sizeof zeros);
    tls_th_hash(s, th);
    empty_hash(hl, emptyhash);
    derive_secret(hl, s->sec.hs, "derived", emptyhash, derived);
    hkdf_extract(hl, derived, hl, zeros, hl, master);
    derive_secret(hl, master, "c ap traffic", th, v->c_ap);
    derive_secret(hl, master, "s ap traffic", th, v->s_ap);
    crypto_wipe(derived, sizeof derived);
    crypto_wipe(master, sizeof master);
}

/* ------------------------------------------------------------- the steps -- */

static int step_send_flight(struct srv_sess *v);

static int step_recv_ch(struct srv_sess *v)
{
    struct tls_sess *s = &v->s;

    /* Drain anything still queued BEFORE reading. This is the only state that
     * can be re-entered with bytes outstanding: a HelloRetryRequest that could
     * not be flushed leaves the retry in s->tx and returns here, and the
     * accumulate loop below only ever calls tls_rec_pull -- so without this the
     * server waits for a ClientHello2 the client cannot send because it never
     * received the retry. Loopback TCP always takes 150 bytes, so no test in
     * this tree can reach it; a real socket under backpressure can. */
    { int fl = tls_tx_flush(s);
      if (fl < 0) return tls_fail(s, TLS_E_TCP);
      if (!fl) return TLS_WANT_WRITE; }

    /* Accumulate handshake bytes until one complete message is buffered. A
     * ClientHello is allowed to be split across records and a padded modern
     * one is ~2 KiB, so "one record is one message" is an assumption that
     * holds until the day it does not. */
    for (;;) {
        if (s->hslen >= 4) {
            int ml = (s->hsbuf[1] << 16) | (s->hsbuf[2] << 8) | s->hsbuf[3];
            if (s->hslen >= 4 + ml) break;
        }
        int r = tls_rec_pull(s);
        if (r == 0) return TLS_WANT_READ;
        if (r < 0) return sfail(v, AL_DECODE_ERROR, TLS_E_TCP);
        const uint8_t *body = s->rxrec + 5;
        int blen = s->reclen;
        uint8_t rt = s->rectype;
        if (rt == REC_CCS) { tls_rec_drop(s); continue; }        /* middlebox compat */
        if (rt == REC_ALERT) { tls_log_alert(body, blen); tls_rec_drop(s); return tls_fail(s, TLS_E_PROTO); }
        if (rt != REC_HANDSHAKE) { tls_rec_drop(s); return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO); }
        if (s->hslen + blen > (int)sizeof s->hsbuf) { tls_rec_drop(s); return sfail(v, AL_DECODE_ERROR, TLS_E_PROTO); }
        memcpy(s->hsbuf + s->hslen, body, (size_t)blen);
        s->hslen += blen;
        tls_rec_drop(s);
    }

    int ml = (s->hsbuf[1] << 16) | (s->hsbuf[2] << 8) | s->hsbuf[3];
    if (s->hsbuf[0] != HS_CLIENT_HELLO) return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO);

    struct ch_info ci;
    if (parse_ch(s->hsbuf + 4, ml, &ci) != 0) return sfail(v, AL_DECODE_ERROR, TLS_E_PROTO);
    if (!ci.v13) {
        /* Refuse rather than fall back. There is no TLS 1.2 SERVER in this
         * tree (tls12.c is a client), so answering 1.2 would start a handshake
         * that cannot finish -- a worse failure than a clean alert, because it
         * fails later and looks like a bug in the peer. */
        kprintf("[tlss] client did not offer TLS 1.3 -- refusing\n");
        return sfail(v, AL_PROTOCOL_VERSION, TLS_E_PROTO);
    }
    if (ci.psk_offered) {
        /* We issue no tickets, so a pre_shared_key can only be for someone
         * else's. Ignoring the extension entirely is the RFC-correct response
         * (the client falls back to the full handshake it also offered); say
         * so once rather than silently, because "resumption silently never
         * happens" is the failure tls.c's own PSK notes warn about. */
        kprintf("[tlss] ignoring pre_shared_key: this server issues no tickets\n");
    }

    if (ci.sidlen > 32) return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO);
    memcpy(v->sid_echo, ci.sid, (size_t)ci.sidlen); v->sid_echolen = ci.sidlen;
    if (ci.sni && ci.snilen > 0 && ci.snilen < (int)sizeof v->sni) {
        memcpy(v->sni, ci.sni, (size_t)ci.snilen); v->sni[ci.snilen] = 0;
    }
    v->nsigalgs = 0;
    for (int i = 0; i + 1 < ci.sigalgslen && v->nsigalgs < (int)(sizeof v->sigalgs / 2); i += 2)
        v->sigalgs[v->nsigalgs++] = (uint16_t)rd_u16(ci.sigalgs + i);

    int suite = select_suite(&ci);
    if (!suite) {
        kprintf("[tlss] no cipher suite in common\n");
        return sfail(v, AL_HANDSHAKE_FAILURE, TLS_E_PROTO);
    }
    /* RFC 8446 4.1.4: the ServerHello's cipher_suite MUST equal the one the
     * HelloRetryRequest named. It is not a formality here -- the transcript was
     * RESTARTED at the retry's hash width (transcript_restart_hrr feeds
     * Hash(CH1) at s->hashlen), so a ClientHello2 that changed the suite would
     * key the schedule on a transcript hashed at the other width and fail two
     * hundred lines later at a Finished MAC. */
    if (v->hrr_sent && suite != s->suite) {
        kprintf("[tlss] ClientHello2 changed the cipher suite -- aborting\n");
        return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO);
    }
    s->suite = suite;
    /* BEFORE any tls_th_hash: this is what picks which of the two running
     * transcripts is the handshake's, exactly as in the client. */
    s->hashlen = tls13_suite_hash(suite);
    s->version = TLS_V13;

    /* The client must be willing to accept the signature our key can make.
     * Signing with an algorithm it did not offer is a handshake it kills at
     * CertificateVerify, where the message blames the certificate. */
    { int want = curve_sigalg(v->ident->key_curve), ok = 0;
      for (int i = 0; i < v->nsigalgs; i++) if (v->sigalgs[i] == want) ok = 1;
      if (!ok) {
          kprintf("[tlss] client does not accept sigalg 0x%x (our P-%d key)\n",
                  want, v->ident->key_curve);
          return sfail(v, AL_HANDSHAKE_FAILURE, TLS_E_PROTO);
      } }

    const uint8_t *cpub = 0; int cpublen = 0;
    int grp = select_keyshare(&ci, &cpub, &cpublen);

    if (!grp) {
        int retry = select_retry_group(&ci);
        if (!retry) {
            kprintf("[tlss] no key exchange group in common\n");
            return sfail(v, AL_HANDSHAKE_FAILURE, TLS_E_PROTO);
        }
        if (v->hrr_sent) {
            /* RFC 8446 4.1.4: exactly one retry. A client that answers a
             * retry without the group it was asked for is looping. */
            kprintf("[tlss] client ignored the HelloRetryRequest\n");
            return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO);
        }
        tls_th_update(s, s->hsbuf, 4 + ml);         /* CH1 into the transcript... */
        s->group = retry;
        transcript_restart_hrr(s);                   /* ...then replaced by its hash */
        uint8_t hrr[512];
        int hn = build_sh(v, hrr, sizeof hrr, 1);
        if (hn < 0) return sfail(v, AL_INTERNAL_ERROR, TLS_E_PROTO);
        if (tls_tx_queue(s, REC_HANDSHAKE, hrr, hn)) return sfail(v, AL_INTERNAL_ERROR, TLS_E_PROTO);
        if (!v->ccs_sent && v->sid_echolen) {
            uint8_t ccs = 1;
            tls_tx_queue(s, REC_CCS, &ccs, 1);
            v->ccs_sent = 1;
        }
        v->hrr_sent = 1;
        s->hslen = 0;                                /* wait for ClientHello2 */
        kprintf("[tlss] HelloRetryRequest: asking for %s\n", tls_group_name(retry));
        int fl = tls_tx_flush(s);
        if (fl < 0) return tls_fail(s, TLS_E_TCP);
        return fl ? step_recv_ch(v) : TLS_WANT_WRITE;
    }

    s->group = grp;
    tls_th_update(s, s->hsbuf, 4 + ml);
    s->hslen = 0;

    select_alpn(v, &ci);

    /* Our ephemeral share, and the shared secret from the client's. */
    if (tls_gen_share(s) != 0) return sfail(v, AL_INTERNAL_ERROR, TLS_E_CRYPTO);
    uint8_t shared[48]; int sharedlen = 0;
    if (tls_compute_shared(s, cpub, cpublen, shared, &sharedlen) != 0) {
        crypto_wipe(shared, sizeof shared);
        /* A share that is the wrong length, off the curve, or (x25519) a
         * low-order point. illegal_parameter, not handshake_failure: the
         * client sent something specific and wrong. */
        return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_CRYPTO);
    }

    rand_bytes(s->random, 32);
    uint8_t sh[512];
    int shn = build_sh(v, sh, sizeof sh, 0);
    if (shn < 0) { crypto_wipe(shared, sizeof shared); return sfail(v, AL_INTERNAL_ERROR, TLS_E_PROTO); }
    if (tls_tx_queue(s, REC_HANDSHAKE, sh, shn)) { crypto_wipe(shared, sizeof shared); return sfail(v, AL_INTERNAL_ERROR, TLS_E_PROTO); }
    if (!v->ccs_sent && v->sid_echolen) {
        uint8_t ccs = 1;
        tls_tx_queue(s, REC_CCS, &ccs, 1);
        v->ccs_sent = 1;
    }

    /* --- handshake secrets. The MIRROR of tls.c's: identical derivations,
     *     opposite assignment. s_hs keys what WE write; c_hs keys what we
     *     read. Getting that backwards produces a handshake that fails at the
     *     peer's Finished with our own MAC looking perfectly correct. --- */
    const int hl = SCHED_HL(s);
    uint8_t zeros[48]; memset(zeros, 0, sizeof zeros);
    uint8_t early[48], derived[48], emptyhash[48];
    hkdf_extract(hl, 0, 0, zeros, hl, early);
    empty_hash(hl, emptyhash);
    derive_secret(hl, early, "derived", emptyhash, derived);
    hkdf_extract(hl, derived, hl, shared, sharedlen, s->sec.hs);
    uint8_t th_chsh[48]; tls_th_hash(s, th_chsh);
    derive_secret(hl, s->sec.hs, "s hs traffic", th_chsh, s->sec.s_hs);
    derive_secret(hl, s->sec.hs, "c hs traffic", th_chsh, s->sec.c_hs);
    traffic_keys(s->sec.s_hs, s->suite, &s->cw);          /* we WRITE with s_hs */
    traffic_keys(s->sec.c_hs, s->suite, &s->cr);          /* we READ  with c_hs */
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(early, sizeof early);
    crypto_wipe(derived, sizeof derived);

    kprintf("[tlss] ClientHello: suite 0x%x, group %s%s%s%s%s\n",
            s->suite, tls_group_name(s->group),
            v->hrr_sent ? " (after HRR)" : "",
            v->sni[0] ? ", sni=" : "", v->sni[0] ? v->sni : "",
            s->alpn_sel[0] ? "" : "");
    if (s->alpn_sel[0]) kprintf("[tlss] alpn=%s\n", s->alpn_sel);

    if (build_flight(v) != 0) return sfail(v, AL_INTERNAL_ERROR, TLS_E_PROTO);
    derive_app_secrets(v);
    v->sub = SS_SEND_FLIGHT;
    return step_send_flight(v);
}

static int step_send_flight(struct srv_sess *v)
{
    struct tls_sess *s = &v->s;
    for (;;) {
        int fl = tls_tx_flush(s);
        if (fl < 0) return tls_fail(s, TLS_E_TCP);
        if (!fl) return TLS_WANT_WRITE;
        if (v->flight_off >= s->hslen) break;
        int chunk = s->hslen - v->flight_off;
        if (chunk > SEND_REC_MAX) chunk = SEND_REC_MAX;
        uint8_t rec[SEND_REC_MAX + 32];
        int rl = aead_seal(&s->cw, REC_HANDSHAKE, s->hsbuf + v->flight_off, chunk, rec);
        if (rl < 0 || tls_tx_queue(s, REC_APPDATA, rec, rl)) return sfail(v, AL_INTERNAL_ERROR, TLS_E_PROTO);
        v->flight_off += chunk;
    }
    /* Everything is on the wire. Our WRITE side switches to the application
     * secret now (RFC 8446 4.4.4: the server may send application data
     * immediately after its Finished); the READ side stays on the handshake
     * secret until the client's Finished arrives under it. */
    traffic_keys(v->s_ap, s->suite, &s->cw);
    s->hslen = 0;
    v->sub = SS_RECV_FIN;
    return TLS_WANT_READ;
}

static int step_recv_fin(struct srv_sess *v)
{
    struct tls_sess *s = &v->s;
    const int hl = SCHED_HL(s);

    for (;;) {
        if (s->hslen >= 4) {
            int ml = (s->hsbuf[1] << 16) | (s->hsbuf[2] << 8) | s->hsbuf[3];
            if (s->hslen >= 4 + ml) break;
        }
        int r = tls_rec_pull(s);
        if (r == 0) return TLS_WANT_READ;
        if (r < 0) return tls_fail(s, TLS_E_TCP);
        const uint8_t *body = s->rxrec + 5;
        int blen = s->reclen;
        uint8_t rt = s->rectype;
        if (rt == REC_CCS) { tls_rec_drop(s); continue; }
        if (rt == REC_ALERT) { tls_log_alert(body, blen); tls_rec_drop(s); return tls_fail(s, TLS_E_PROTO); }
        if (rt != REC_APPDATA) { tls_rec_drop(s); return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO); }
        if (blen - 16 > (int)sizeof s->app) { tls_rec_drop(s); return sfail(v, AL_DECODE_ERROR, TLS_E_PROTO); }
        uint8_t it;
        int dl = aead_open(&s->cr, body, blen, s->app, &it);
        tls_rec_drop(s);
        if (dl < 0) return sfail(v, AL_DECRYPT_ERROR, TLS_E_CRYPTO);
        if (it == REC_ALERT) { tls_log_alert(s->app, dl); return tls_fail(s, TLS_E_PROTO); }
        if (it != REC_HANDSHAKE) continue;
        if (s->hslen + dl > (int)sizeof s->hsbuf) return sfail(v, AL_DECODE_ERROR, TLS_E_PROTO);
        memcpy(s->hsbuf + s->hslen, s->app, (size_t)dl);
        s->hslen += dl;
    }

    int ml = (s->hsbuf[1] << 16) | (s->hsbuf[2] << 8) | s->hsbuf[3];
    if (s->hsbuf[0] != HS_FINISHED || ml != hl)
        return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO);
    /* Nothing may follow it. Without client authentication the client's flight
     * IS the Finished, so a trailing message is either a post-handshake type we
     * do not implement (KeyUpdate) or a client Certificate we did not ask for.
     * Dropping it silently -- which `s->hslen = 0` below would do -- is the
     * shape this tree keeps finding: the mechanism looks fine and the message
     * is gone. Refuse instead. */
    if (s->hslen != 4 + ml) {
        kprintf("[tlss] %d unexpected bytes after the client Finished\n", s->hslen - 4 - ml);
        return sfail(v, AL_ILLEGAL_PARAMETER, TLS_E_PROTO);
    }

    /* The transcript for the client's Finished runs ClientHello..server
     * Finished -- i.e. exactly what it was when the flight was built, which is
     * why nothing has been added to it since. */
    uint8_t th[48]; tls_th_hash(s, th);
    uint8_t fk[48], expect[48];
    hkdf_expand_label(hl, s->sec.c_hs, "finished", 0, 0, fk, hl);
    hmac(hl, fk, hl, th, hl, expect);
    int bad = memcmp(expect, s->hsbuf + 4, (size_t)hl) != 0;
    crypto_wipe(fk, sizeof fk); crypto_wipe(expect, sizeof expect);
    if (bad) {
        kprintf("[tlss] client Finished did not verify\n");
        return sfail(v, AL_DECRYPT_ERROR, TLS_E_CRYPTO);
    }

    traffic_keys(v->c_ap, s->suite, &s->cr);      /* the read side switches now */
    crypto_wipe(&s->sec, sizeof s->sec);
    crypto_wipe(v->s_ap, sizeof v->s_ap);
    crypto_wipe(v->c_ap, sizeof v->c_ap);
    crypto_wipe(s->priv, sizeof s->priv);
    s->hslen = 0;
    v->sub = SS_ESTABLISHED;
    tlsprof_close(&s->prof);
    kprintf("[tlss] handshake complete: suite 0x%x, group %s%s%s\n",
            s->suite, tls_group_name(s->group),
            s->alpn_sel[0] ? ", alpn=" : "", s->alpn_sel[0] ? s->alpn_sel : "");
    return TLS_DONE;
}

/* ------------------------------------------------------------ public API -- */

int tlss_start(int tcp_id, const struct tls_ident *ident, const char *alpn, int64_t now)
{
    if (!ident || ident->nchain < 1 || ident->nchain > TLSS_CHAIN_MAX) return TLS_E_PROTO;
    if (ident->key_curve != 256 && ident->key_curve != 384 && ident->key_curve != 521)
        return TLS_E_PROTO;
    int total = 0;
    for (int i = 0; i < ident->nchain; i++) {
        if (ident->chainlen[i] < 1) return TLS_E_PROTO;
        total += 5 + ident->chainlen[i];
    }
    /* Bounded HERE rather than discovered halfway through building the flight,
     * where the failure would be an internal_error alert at handshake time on
     * every connection instead of a refusal at configuration time on one. */
    if (total > TLSS_CHAIN_BYTES) {
        kprintf("[tlss] certificate chain is %d bytes, over the %d limit\n",
                total, TLSS_CHAIN_BYTES);
        return TLS_E_PROTO;
    }
    if (!rng_strong()) {
        kprintf("[tlss] refusing handshake: weak RNG (no rdrand/rdseed)\n");
        return TLS_E_CRYPTO;
    }

    int id = -1;
    for (int i = 0; i < TLSS_MAX_SESSIONS; i++) if (!ssn[i].used) { id = i; break; }
    if (id < 0) return TLS_E_PROTO;
    struct srv_sess *v = &ssn[id];
    memset(v, 0, sizeof *v);
    struct tls_sess *s = &v->s;
    tlsprof_open(&s->prof, &srv_prof_slot, "tls_server_handshake");
    v->used = 1; v->ident = ident; v->sub = SS_RECV_CH;
    s->used = 1; s->tcp = tcp_id; s->now = now;
    /* The same absolute budget the client uses, for the same reason: a peer
     * trickling one byte at a time must not be able to hold a session slot
     * open forever by resetting a per-record idle timer. It matters MORE on a
     * server, where the peer is unauthenticated and the slots are four. */
    s->deadline = timer_ticks() + 3000;
    if (alpn) {
        int i = 0;
        while (alpn[i] && i < (int)sizeof s->alpn_offer - 1) { s->alpn_offer[i] = alpn[i]; i++; }
        s->alpn_offer[i] = 0;
    }
    s->hashlen = 32;
    sha256_init(&s->th);
    sha384_init(&s->th384);
    return id;
}

int tlss_step(int id)
{
    struct srv_sess *v = sess_of(id);
    if (!v) return TLS_E_PROTO;
    struct tls_sess *s = &v->s;
    if (v->sub == SS_FAILED) return s->err ? s->err : TLS_E_PROTO;
    if (v->sub == SS_ESTABLISHED) return TLS_DONE;
    if ((int64_t)(timer_ticks() - s->deadline) > 0) {
        kprintf("[tlss] handshake timed out\n");
        return sfail(v, AL_INTERNAL_ERROR, TLS_E_TCP);
    }
    switch (v->sub) {
    case SS_RECV_CH:     return step_recv_ch(v);
    case SS_SEND_FLIGHT: return step_send_flight(v);
    case SS_RECV_FIN:    return step_recv_fin(v);
    default:             return TLS_E_PROTO;
    }
}

int tlss_send(int id, const void *buf, int len)
{
    struct srv_sess *v = sess_of(id);
    if (!v || v->sub != SS_ESTABLISHED) return -1;
    struct tls_sess *s = &v->s;
    int fl = tls_tx_flush(s);
    if (fl < 0) { tls_fail(s, TLS_E_TCP); v->sub = SS_FAILED; return -1; }
    if (!fl) return 0;                               /* a whole record is still pending */
    if (len > SEND_REC_MAX) len = SEND_REC_MAX;
    uint8_t rec[SEND_REC_MAX + 32];
    int rl = aead_seal(&s->cw, REC_APPDATA, buf, len, rec);
    if (rl < 0 || tls_tx_queue(s, REC_APPDATA, rec, rl)) return -1;
    if (tls_tx_flush(s) < 0) { tls_fail(s, TLS_E_TCP); v->sub = SS_FAILED; return -1; }
    return len;
}

int tlss_recv(int id, void *buf, int max)
{
    struct srv_sess *v = sess_of(id);
    if (!v || v->sub != SS_ESTABLISHED) return -1;
    struct tls_sess *s = &v->s;
    /* Push anything still queued. tlss_send returns "accepted" once bytes are
     * in s->tx, so a caller that sends and then only ever polls for a reply
     * would otherwise never get the last record onto the wire -- and would read
     * that as the peer having gone quiet. */
    if (tls_tx_flush(s) < 0) { tls_fail(s, TLS_E_TCP); v->sub = SS_FAILED; return -1; }
    for (;;) {
        if (s->applen > s->appoff) {
            int n = s->applen - s->appoff;
            if (n > max) n = max;
            memcpy(buf, s->app + s->appoff, (size_t)n);
            s->appoff += n;
            if (s->appoff >= s->applen) s->applen = s->appoff = 0;
            return n;
        }
        int r = tls_rec_pull(s);
        if (r == 0) return 0;
        if (r < 0) return -1;
        const uint8_t *body = s->rxrec + 5;
        int blen = s->reclen;
        uint8_t rt = s->rectype;
        if (rt == REC_CCS) { tls_rec_drop(s); continue; }
        if (rt != REC_APPDATA) { tls_rec_drop(s); return -1; }
        if (blen - 16 > (int)sizeof s->app) { tls_rec_drop(s); return -1; }
        uint8_t it;
        int dl = aead_open(&s->cr, body, blen, s->app, &it);
        tls_rec_drop(s);
        if (dl < 0) { tls_fail(s, TLS_E_CRYPTO); v->sub = SS_FAILED; return -1; }
        if (it == REC_ALERT) {
            /* close_notify is an orderly shutdown, not a failure -- but the
             * session is over either way, so both return -1. Logging tells
             * them apart for whoever is reading. */
            tls_log_alert(s->app, dl);
            v->sub = SS_FAILED; s->err = TLS_E_PROTO;
            return -1;
        }
        if (it == REC_HANDSHAKE) {
            /* Post-handshake messages: a client may send KeyUpdate. We do not
             * implement it, and answering "fine" to a key update we did not
             * perform would desynchronise the record layer silently. Refuse
             * out loud instead. */
            kprintf("[tlss] post-handshake message (type %d) is not supported\n",
                    dl > 0 ? s->app[0] : -1);
            srv_alert(v, AL_HANDSHAKE_FAILURE);
            tls_fail(s, TLS_E_PROTO); v->sub = SS_FAILED;
            return -1;
        }
        if (it != REC_APPDATA) continue;
        s->applen = dl; s->appoff = 0;
    }
}

int tlss_alpn(int id, char *out, int max)
{
    struct srv_sess *v = sess_of(id);
    if (!v || max < 1) return -1;
    int n = 0; while (v->s.alpn_sel[n] && n < max - 1) { out[n] = v->s.alpn_sel[n]; n++; }
    out[n] = 0;
    return n;
}

int tlss_sni(int id, char *out, int max)
{
    struct srv_sess *v = sess_of(id);
    if (!v || max < 1) return -1;
    int n = 0; while (v->sni[n] && n < max - 1) { out[n] = v->sni[n]; n++; }
    out[n] = 0;
    return n;
}

int tlss_version(int id)
{
    struct srv_sess *v = sess_of(id);
    return v ? v->s.version : 0;
}

int tlss_pending(int id)
{
    struct srv_sess *v = sess_of(id);
    if (!v || v->sub != SS_ESTABLISHED) return 0;
    return v->s.applen - v->s.appoff;
}

void tlss_close(int id)
{
    struct srv_sess *v = sess_of(id);
    if (!v) return;
    if (v->sub == SS_ESTABLISHED) srv_alert_lv(v, 1 /* warning */, AL_CLOSE_NOTIFY);
    crypto_wipe(&v->s.sec, sizeof v->s.sec);
    crypto_wipe(&v->s.cr, sizeof v->s.cr);
    crypto_wipe(&v->s.cw, sizeof v->s.cw);
    crypto_wipe(v->s.priv, sizeof v->s.priv);
    crypto_wipe(v->s_ap, sizeof v->s_ap);
    crypto_wipe(v->c_ap, sizeof v->c_ap);
    memset(v, 0, sizeof *v);
}
