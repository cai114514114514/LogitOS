#include <stdint.h>
#include <stddef.h>
#include "ocsp.h"
#include "x509.h"
#include "crypto.h"

int memcmp(const void *, const void *, size_t);

/* See ocsp.h for the policy this implements and for why there is no online
 * fetch. This file is the parser and the verifier.
 *
 * SHA-1 lives in c/crypto/hash/sha1.c and is declared HERE rather than in
 * crypto.h, on purpose: it exists only to compute a CertID lookup key, and the
 * long comment at the top of that file is the argument for why that use is
 * sound and why a second caller would not be. */
void ocsp_sha1(const void *data, size_t len, uint8_t out[20]);

/* --- the same minimal DER reader shape x509.c uses ------------------------
 * Duplicated rather than shared. x509.c's reader is static and its file is one
 * of the two or three most security-critical in the tree; exporting its
 * internals to give this file a header would widen that file's surface to buy
 * forty lines. The reader is small enough that a second copy is cheaper to
 * audit than a new coupling. */
struct der { const uint8_t *p, *end; };

static int der_tlv(struct der *d, int *tag, const uint8_t **content, int *clen)
{
    if (d->p >= d->end) return -1;
    *tag = *d->p++;
    if (d->p >= d->end) return -1;
    int len = *d->p++;
    if (len & 0x80) {
        int nb = len & 0x7f;
        if (nb == 0 || nb > 3 || d->p + nb > d->end) return -1;
        len = 0;
        while (nb--) len = (len << 8) | *d->p++;
    }
    if (len < 0 || d->p + len > d->end) return -1;
    *content = d->p; *clen = len;
    d->p += len;
    return 0;
}

static int der_enter(struct der *d, int want_tag, struct der *sub)
{
    int tag, clen; const uint8_t *c;
    if (der_tlv(d, &tag, &c, &clen)) return -1;
    if (want_tag >= 0 && tag != want_tag) return -1;
    sub->p = c; sub->end = c + clen;
    return 0;
}

/* A TLV's whole span (tag+len+content), which is what has to be hashed for a
 * signed region. */
static int der_span(struct der *d, const uint8_t **start, int *spanlen)
{
    const uint8_t *s = d->p;
    int tag, clen; const uint8_t *c;
    if (der_tlv(d, &tag, &c, &clen)) return -1;
    *start = s; *spanlen = (int)(d->p - s);
    return 0;
}

static const uint8_t OID_OCSP_BASIC[]  = {0x2b,0x06,0x01,0x05,0x05,0x07,0x30,0x01,0x01};
static const uint8_t OID_SHA1_ALG[]    = {0x2b,0x0e,0x03,0x02,0x1a};                    /* 1.3.14.3.2.26 */
static const uint8_t OID_SHA256_ALG[]  = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01};
static const uint8_t OID_ECDSA_SHA256[]= {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_SHA384[]= {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const uint8_t OID_ECDSA_SHA512[]= {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04};
static const uint8_t OID_RSA_SHA256[]  = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const uint8_t OID_RSA_SHA384[]  = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
static const uint8_t OID_RSA_SHA512[]  = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d};

static int oid_eq(const uint8_t *o, int ol, const uint8_t *w, int wl)
{ return ol == wl && memcmp(o, w, (size_t)wl) == 0; }

/* GeneralizedTime -> unix-ish seconds. OCSP always uses GeneralizedTime
 * (RFC 6960 4.2.2.1), so unlike x509.c there is no UTCTime case. A field we
 * cannot read returns 0, which the caller treats as "not current" rather than
 * as "1970" -- see the check in single_response(). */
static int64_t gen_time(const uint8_t *p, int len)
{
    if (len < 14) return 0;
    for (int i = 0; i < 14; i++) if (p[i] < '0' || p[i] > '9') return 0;
    int yr = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int mon= (p[4]-'0')*10 + (p[5]-'0');
    int day= (p[6]-'0')*10 + (p[7]-'0');
    int hh = (p[8]-'0')*10 + (p[9]-'0');
    int mm = (p[10]-'0')*10 + (p[11]-'0');
    int ss = (p[12]-'0')*10 + (p[13]-'0');
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
    int64_t days = 0;
    for (int y = 1970; y < yr; y++) days += (y%4==0 && (y%100!=0 || y%400==0)) ? 366 : 365;
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 1; m < mon; m++) {
        days += md[m-1];
        if (m == 2 && (yr%4==0 && (yr%100!=0 || yr%400==0))) days++;
    }
    days += day - 1;
    return ((days*24 + hh)*60 + mm)*60 + ss;
}

/* Verify `sig` over `msg` using `signer`'s public key, dispatching on the
 * AlgorithmIdentifier OID. The same EC/RSA split x509_verify_signed_by does;
 * it is not shared because that function's signature is (child, issuer) over a
 * certificate's tbs and this one signs an arbitrary span. */
static int verify_by(const struct cert *signer, const uint8_t *algoid, int alglen,
                     const uint8_t *msg, int msglen, const uint8_t *sig, int siglen)
{
    int hlen = 0, is_ec = 0;
    if      (oid_eq(algoid,alglen,OID_ECDSA_SHA256,sizeof OID_ECDSA_SHA256)) { hlen=32; is_ec=1; }
    else if (oid_eq(algoid,alglen,OID_ECDSA_SHA384,sizeof OID_ECDSA_SHA384)) { hlen=48; is_ec=1; }
    else if (oid_eq(algoid,alglen,OID_ECDSA_SHA512,sizeof OID_ECDSA_SHA512)) { hlen=64; is_ec=1; }
    else if (oid_eq(algoid,alglen,OID_RSA_SHA256,sizeof OID_RSA_SHA256))     { hlen=32; }
    else if (oid_eq(algoid,alglen,OID_RSA_SHA384,sizeof OID_RSA_SHA384))     { hlen=48; }
    else if (oid_eq(algoid,alglen,OID_RSA_SHA512,sizeof OID_RSA_SHA512))     { hlen=64; }
    else return 0;                        /* including every SHA-1 algorithm */

    uint8_t h[64];
    if (hlen == 32) sha256(msg, (size_t)msglen, h);
    else if (hlen == 48) sha384(msg, (size_t)msglen, h);
    else sha512(msg, (size_t)msglen, h);

    if (is_ec) {
        if (signer->key_type != KEY_EC) return 0;
        int flen = x509_ec_flen(signer->key_curve);
        if (!flen || signer->publen != 1 + 2*flen || signer->pub[0] != 0x04) return 0;
        uint8_t rs[132];
        if (x509_der_sig_to_rs(sig, siglen, rs, flen)) return 0;
        return ecdsa_verify(signer->key_curve, signer->pub + 1, rs, h, hlen);
    }
    if (signer->key_type != KEY_RSA) return 0;
    return rsa_pkcs1_verify(signer->rsa_n, signer->rsa_nlen,
                            signer->rsa_e, signer->rsa_elen, sig, siglen, h, hlen);
}

/* Does this CertID name `leaf` (issued by `issuer`)?
 * RFC 6960 4.1.1:
 *   issuerNameHash = H(issuer's Subject Name, DER, the whole TLV)
 *   issuerKeyHash  = H(issuer's subjectPublicKey BIT STRING contents, unused-
 *                      bits octet removed)   <- NOT the whole SPKI
 *   serialNumber   = the leaf's serial, as a DER INTEGER
 * Getting either hash preimage wrong produces "no match" on every real
 * response, which looks exactly like a server that stapled the wrong thing --
 * so both are spelled out here rather than left to a reader of the RFC. */
static int certid_matches(struct der *cid, const struct cert *leaf, const struct cert *issuer)
{
    struct der alg;
    if (der_enter(cid, 0x30, &alg)) return 0;
    int g; const uint8_t *oc; int ol;
    if (der_tlv(&alg, &g, &oc, &ol)) return 0;

    int hlen;
    if      (oid_eq(oc,ol,OID_SHA1_ALG,sizeof OID_SHA1_ALG))   hlen = 20;
    else if (oid_eq(oc,ol,OID_SHA256_ALG,sizeof OID_SHA256_ALG)) hlen = 32;
    else return 0;                        /* an algorithm we cannot compute */

    const uint8_t *nh; int nhl;
    const uint8_t *kh; int khl;
    const uint8_t *sn; int snl;
    if (der_tlv(cid, &g, &nh, &nhl) || g != 0x04) return 0;    /* issuerNameHash */
    if (der_tlv(cid, &g, &kh, &khl) || g != 0x04) return 0;    /* issuerKeyHash  */
    if (der_tlv(cid, &g, &sn, &snl) || g != 0x02) return 0;    /* serialNumber   */

    if (nhl != hlen || khl != hlen) return 0;

    uint8_t want[32];
    if (hlen == 20) ocsp_sha1(issuer->subject, (size_t)issuer->subjectlen, want);
    else            sha256(issuer->subject, (size_t)issuer->subjectlen, want);
    if (memcmp(want, nh, (size_t)hlen) != 0) return 0;

    if (!issuer->spki_key || issuer->spki_keylen <= 0) return 0;
    if (hlen == 20) ocsp_sha1(issuer->spki_key, (size_t)issuer->spki_keylen, want);
    else            sha256(issuer->spki_key, (size_t)issuer->spki_keylen, want);
    if (memcmp(want, kh, (size_t)hlen) != 0) return 0;

    if (snl != leaf->seriallen || !leaf->serial) return 0;
    return memcmp(sn, leaf->serial, (size_t)snl) == 0;
}

const char *ocsp_strerror(int rc)
{
    switch (rc) {
    case OCSP_OK:         return "good";
    case OCSP_E_PARSE:    return "malformed response";
    case OCSP_E_STATUS:   return "responder returned an error status";
    case OCSP_E_TYPE:     return "not a basic OCSP response";
    case OCSP_E_CERTID:   return "response is about a different certificate";
    case OCSP_E_REVOKED:  return "CERTIFICATE REVOKED";
    case OCSP_E_UNKNOWN:  return "responder does not know this certificate";
    case OCSP_E_STALE:    return "response is stale or not yet valid";
    case OCSP_E_SIG:      return "response signature is invalid";
    case OCSP_E_SIGNER:   return "response signed by nobody entitled to";
    default:              return "?";
    }
}

/* Test hook. Report where tbsResponseData -- the SIGNED region -- sits inside
 * the DER, so tests/unit/ocsp_test.c can flip every bit of it and require a
 * refusal for each.
 *
 * It exists because the obvious version of that test is wrong. Several bytes of
 * a real OCSPResponse are genuinely free: the outer SEQUENCE's length has slack
 * once a trailing certificate is dropped, the signatureAlgorithm's NULL
 * parameters are read by nobody (only the OID is), and the certificate bag
 * holds the ISSUER's own certificate, which this verifier never consults
 * because the issuer comes from the chain. A sweep over the whole file that
 * demanded every byte matter would be asserting a property no correct
 * implementation has -- and the way to make it pass would be to add checks that
 * buy nothing. So the sweep is aimed exactly at the bytes the signature covers,
 * and this is how the test finds them. Returns 0 on success. */
int ocsp_signed_span(const uint8_t *der, int len, int *off, int *span)
{
    if (!der || len <= 0 || !off || !span) return -1;
    struct der top; top.p = der; top.end = der + len;
    struct der resp;
    if (der_enter(&top, 0x30, &resp)) return -1;
    int g; const uint8_t *c; int cl;
    if (der_tlv(&resp, &g, &c, &cl) || g != 0x0a) return -1;
    struct der rb;  if (der_enter(&resp, 0xA0, &rb)) return -1;
    struct der rbs; if (der_enter(&rb, 0x30, &rbs)) return -1;
    if (der_tlv(&rbs, &g, &c, &cl) || g != 0x06) return -1;
    const uint8_t *basic; int basiclen;
    if (der_tlv(&rbs, &g, &basic, &basiclen) || g != 0x04) return -1;
    struct der bt; bt.p = basic; bt.end = basic + basiclen;
    struct der br; if (der_enter(&bt, 0x30, &br)) return -1;
    const uint8_t *tbs; int tbslen;
    if (der_span(&br, &tbs, &tbslen)) return -1;
    *off = (int)(tbs - der);
    *span = tbslen;
    return 0;
}

int ocsp_check(const uint8_t *der, int len,
               const struct cert *leaf, const struct cert *issuer,
               int64_t now, int64_t skew)
{
    if (!der || len <= 0 || !leaf || !issuer) return OCSP_E_PARSE;

    struct der top; top.p = der; top.end = der + len;
    struct der resp;
    if (der_enter(&top, 0x30, &resp)) return OCSP_E_PARSE;      /* OCSPResponse */

    /* responseStatus ENUMERATED. 0 = successful; anything else is the
     * responder declining, and it is NOT signed -- which is precisely why a
     * non-zero status can never be allowed to mean "assume good". */
    int g; const uint8_t *c; int cl;
    if (der_tlv(&resp, &g, &c, &cl) || g != 0x0a || cl != 1) return OCSP_E_PARSE;
    if (c[0] != 0) return OCSP_E_STATUS;

    struct der rb;
    if (der_enter(&resp, 0xA0, &rb)) return OCSP_E_PARSE;       /* [0] responseBytes */
    struct der rbs;
    if (der_enter(&rb, 0x30, &rbs)) return OCSP_E_PARSE;
    if (der_tlv(&rbs, &g, &c, &cl) || g != 0x06) return OCSP_E_PARSE;
    if (!oid_eq(c, cl, OID_OCSP_BASIC, sizeof OID_OCSP_BASIC)) return OCSP_E_TYPE;
    const uint8_t *basic; int basiclen;
    if (der_tlv(&rbs, &g, &basic, &basiclen) || g != 0x04) return OCSP_E_PARSE;

    struct der bt; bt.p = basic; bt.end = basic + basiclen;
    struct der br;
    if (der_enter(&bt, 0x30, &br)) return OCSP_E_PARSE;         /* BasicOCSPResponse */

    /* tbsResponseData: the SIGNED region is its whole TLV, so it is taken as a
     * span before being parsed. Re-encoding it from the parsed fields would be
     * the classic way to verify a signature over bytes that are not the bytes
     * that arrived. */
    const uint8_t *tbs; int tbslen;
    {
        struct der peek = br;
        if (der_span(&peek, &tbs, &tbslen)) return OCSP_E_PARSE;
    }
    struct der rd;
    if (der_enter(&br, 0x30, &rd)) return OCSP_E_PARSE;         /* ResponseData */

    /* signatureAlgorithm + signature, read now because the signer search below
     * needs them. */
    struct der sa;
    if (der_enter(&br, 0x30, &sa)) return OCSP_E_PARSE;
    const uint8_t *algoid; int alglen;
    if (der_tlv(&sa, &g, &algoid, &alglen) || g != 0x06) return OCSP_E_PARSE;
    const uint8_t *sigbits; int sigbitslen;
    if (der_tlv(&br, &g, &sigbits, &sigbitslen) || g != 0x03) return OCSP_E_PARSE;
    if (sigbitslen < 2 || sigbits[0] != 0) return OCSP_E_PARSE; /* 0 unused bits */
    const uint8_t *sig = sigbits + 1; int siglen = sigbitslen - 1;

    /* --- who is allowed to have signed this? ------------------------------
     * Two, and only two (RFC 6960 4.2.2.2):
     *   (a) the issuer of the certificate in question, with its own key; or
     *   (b) a certificate the ISSUER signed, carried in this response, that
     *       carries id-kp-OCSPSigning in extKeyUsage.
     * The EKU check in (b) is the whole of the delegation's safety. Without it
     * any leaf the CA ever issued -- including one an attacker legitimately
     * bought from the same CA -- could sign "good" for a revoked certificate.
     * A "trust the first cert in the bag" implementation is that bug. */
    int signer_ok = 0, saw_candidates = 0;

    if (verify_by(issuer, algoid, alglen, tbs, tbslen, sig, siglen)) {
        signer_ok = 1;                                  /* (a) directly by the issuer */
    } else if (br.p < br.end && br.p[0] == 0xA0) {
        struct der certs0, certs;
        if (der_enter(&br, 0xA0, &certs0) == 0 && der_enter(&certs0, 0x30, &certs) == 0) {
            saw_candidates = 1;
            while (certs.p < certs.end && !signer_ok) {
                const uint8_t *cs; int csl;
                if (der_span(&certs, &cs, &csl)) break;
                struct cert responder;
                if (x509_parse(cs, csl, &responder) != 0) continue;
                if (!responder.eku_ocsp_signing) continue;      /* the delegation check */
                if (x509_verify_signed_by(&responder, issuer) != 0) continue;
                if (responder.not_before > now + skew || responder.not_after < now - skew) continue;
                if (verify_by(&responder, algoid, alglen, tbs, tbslen, sig, siglen))
                    signer_ok = 1;
            }
        }
    }
    if (!signer_ok) {
        /* Distinguish "nothing was entitled to sign this" from "the signature
         * is simply wrong". Both are fatal and the handshake dies either way;
         * the difference is what a log tells whoever reads it. A response that
         * OFFERED delegated certificates and had none of them qualify is a
         * SIGNER problem -- most often the id-kp-OCSPSigning check refusing a
         * certificate the CA did issue, which is the interesting case. A
         * response with no bag at all could only have been signed by the
         * issuer, so a failure there is simply a bad signature. */
        return saw_candidates ? OCSP_E_SIGNER : OCSP_E_SIG;
    }

    /* --- ResponseData: version [0], responderID, producedAt, responses ---- */
    if (rd.p < rd.end && rd.p[0] == 0xA0) { if (der_tlv(&rd,&g,&c,&cl)) return OCSP_E_PARSE; }
    /* responderID CHOICE: [1] byName or [2] byKey. Read past it -- WHICH
     * responder it names does not matter, because the signature check above has
     * already decided whether the key that signed is one entitled to. */
    if (der_tlv(&rd, &g, &c, &cl) || (g != 0xA1 && g != 0xA2)) return OCSP_E_PARSE;
    if (der_tlv(&rd, &g, &c, &cl) || g != 0x18) return OCSP_E_PARSE;   /* producedAt */

    struct der responses;
    if (der_enter(&rd, 0x30, &responses)) return OCSP_E_PARSE;

    int found = OCSP_E_CERTID;
    while (responses.p < responses.end) {
        struct der sr;
        if (der_enter(&responses, 0x30, &sr)) return OCSP_E_PARSE;   /* SingleResponse */
        struct der cid;
        if (der_enter(&sr, 0x30, &cid)) return OCSP_E_PARSE;         /* CertID */
        if (!certid_matches(&cid, leaf, issuer)) continue;

        /* certStatus CHOICE: [0] good (NULL), [1] revoked, [2] unknown. */
        const uint8_t *sc; int scl; int stag;
        if (der_tlv(&sr, &stag, &sc, &scl)) return OCSP_E_PARSE;
        if (stag == 0xA1 || stag == 0x81) return OCSP_E_REVOKED;
        if (stag == 0xA2 || stag == 0x82) return OCSP_E_UNKNOWN;
        if (stag != 0x80 && stag != 0xA0) return OCSP_E_PARSE;

        /* thisUpdate, then optional [0] nextUpdate. */
        const uint8_t *tu; int tul;
        if (der_tlv(&sr, &g, &tu, &tul) || g != 0x18) return OCSP_E_PARSE;
        int64_t this_update = gen_time(tu, tul);
        if (this_update == 0) return OCSP_E_PARSE;
        if (this_update > now + skew) return OCSP_E_STALE;   /* signed in the future */

        int64_t next_update = 0;
        if (sr.p < sr.end && sr.p[0] == 0xA0) {
            struct der nu;
            if (der_enter(&sr, 0xA0, &nu)) return OCSP_E_PARSE;
            const uint8_t *nc; int ncl;
            if (der_tlv(&nu, &g, &nc, &ncl) || g != 0x18) return OCSP_E_PARSE;
            next_update = gen_time(nc, ncl);
            if (next_update == 0) return OCSP_E_PARSE;
        }

        /* nextUpdate is OPTIONAL in the protocol and REQUIRED here.
         * RFC 6960 2.4 says its absence means the responder always has newer
         * information available -- which is fine for a live query and useless
         * for a STAPLED one, where the server chooses how old a response to
         * hand out. Without a nextUpdate a server could staple a two-year-old
         * "good" forever, which is exactly the attack revocation exists to
         * stop. Every real stapling CA emits it. */
        if (next_update == 0) return OCSP_E_STALE;
        if (next_update < now - skew) return OCSP_E_STALE;

        found = OCSP_OK;
        break;
    }
    return found;
}
