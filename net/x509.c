#include <stdint.h>
#include <stddef.h>
#include "x509.h"
#include "crypto.h"

int memcmp(const void *, const void *, size_t);

/* --- minimal DER reader --- */
struct der { const uint8_t *p, *end; };

/* Read one TLV; *tag/*content/*clen set; advance past it. 0 ok, -1 error. */
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
    if (d->p + len > d->end) return -1;
    *content = d->p; *clen = len;
    d->p += len;
    return 0;
}

/* Enter a constructed TLV (SEQUENCE/SET/[n]): returns a sub-reader over content. */
static int der_enter(struct der *d, int want_tag, struct der *sub)
{
    int tag, clen; const uint8_t *c;
    if (der_tlv(d, &tag, &c, &clen)) return -1;
    if (want_tag >= 0 && tag != want_tag) return -1;
    sub->p = c; sub->end = c + clen;
    return 0;
}

/* OIDs we care about (raw DER content bytes, without tag/len). */
static const uint8_t OID_ECDSA_SHA256[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_SHA384[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const uint8_t OID_EC_PUBKEY[]    = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const uint8_t OID_P256[]         = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const uint8_t OID_P384[]         = {0x2b,0x81,0x04,0x00,0x22};
static const uint8_t OID_CN[]           = {0x55,0x04,0x03};
static const uint8_t OID_SAN[]          = {0x55,0x1d,0x11};

static int oid_eq(const uint8_t *o, int olen, const uint8_t *want, int wlen)
{ return olen == wlen && memcmp(o, want, wlen) == 0; }

/* Parse a 2-digit field from ASN.1 time. */
static int two(const uint8_t *p) { return (p[0]-'0')*10 + (p[1]-'0'); }

/* Convert UTCTime/GeneralizedTime to a rough unix-ish seconds value (good enough
 * for not-before/not-after ordering checks). */
static int64_t parse_time(int tag, const uint8_t *p, int len)
{
    int year, i = 0;
    if (tag == 0x17) { year = 2000 + two(p); i = 2; }   /* UTCTime YY */
    else { year = two(p)*100 + two(p+2); i = 4; }       /* GeneralizedTime YYYY */
    int mon = two(p+i), day = two(p+i+2), hh = two(p+i+4), mm = two(p+i+6), ss = two(p+i+8);
    (void)len;
    /* days since 1970 (proleptic Gregorian, no leap-second fuss) */
    int64_t days = 0;
    for (int y = 1970; y < year; y++) days += (y%4==0 && (y%100!=0 || y%400==0)) ? 366 : 365;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 1; m < mon; m++) { days += mdays[m-1]; if (m==2 && (year%4==0 && (year%100!=0||year%400==0))) days++; }
    days += day - 1;
    return ((days*24 + hh)*60 + mm)*60 + ss;
}

int x509_parse(const uint8_t *der, int len, struct cert *out)
{
    out->der = der; out->derlen = len;
    out->cn = 0; out->cnlen = 0; out->san = 0; out->sanlen = 0;

    struct der top, cert;
    top.p = der; top.end = der + len;
    if (der_enter(&top, 0x30, &cert)) return X509_E_PARSE;        /* Certificate */

    /* tbsCertificate: capture its full TLV span as the signed region */
    const uint8_t *tbs_start = cert.p;
    struct der tbs;
    if (der_enter(&cert, 0x30, &tbs)) return X509_E_PARSE;
    out->tbs = tbs_start; out->tbslen = (int)(tbs.end - tbs_start);

    /* tbs: [0] version?, serial, sigalg, issuer, validity, subject, spki, exts */
    int tag, clen; const uint8_t *c;
    if (tbs.p < tbs.end && tbs.p[0] == 0xA0) { if (der_tlv(&tbs,&tag,&c,&clen)) return X509_E_PARSE; } /* version */
    if (der_tlv(&tbs,&tag,&c,&clen)) return X509_E_PARSE;        /* serial */
    /* signature algorithm (inside tbs) */
    { struct der sa; struct der t=tbs; if (der_enter(&t,0x30,&sa)) return X509_E_PARSE;
      int g; const uint8_t *oc; int ol; if (der_tlv(&sa,&g,&oc,&ol)) return X509_E_PARSE;
      tbs = t; }
    /* issuer Name (raw) */
    { const uint8_t *s=tbs.p; struct der nm; if (der_enter(&tbs,0x30,&nm)) return X509_E_PARSE;
      out->issuer=s; out->issuerlen=(int)(nm.end - s); }
    /* validity: SEQ { notBefore, notAfter } */
    { struct der va; if (der_enter(&tbs,0x30,&va)) return X509_E_PARSE;
      int g; const uint8_t *tc; int tl;
      if (der_tlv(&va,&g,&tc,&tl)) return X509_E_PARSE; out->not_before=parse_time(g,tc,tl);
      if (der_tlv(&va,&g,&tc,&tl)) return X509_E_PARSE; out->not_after =parse_time(g,tc,tl); }
    /* subject Name (raw) + pull CN */
    { const uint8_t *s=tbs.p; struct der nm; if (der_enter(&tbs,0x30,&nm)) return X509_E_PARSE;
      out->subject=s; out->subjectlen=(int)(nm.end - s);
      struct der rdns=nm;
      while (rdns.p < rdns.end) {                                /* RDNSequence */
          struct der set; if (der_enter(&rdns,0x31,&set)) break;
          struct der atv; if (der_enter(&set,0x30,&atv)) continue;
          int g; const uint8_t *oc; int ol; if (der_tlv(&atv,&g,&oc,&ol)) continue;
          int g2; const uint8_t *vc; int vl; if (der_tlv(&atv,&g2,&vc,&vl)) continue;
          if (oid_eq(oc,ol,OID_CN,sizeof OID_CN)) { out->cn=(const char*)vc; out->cnlen=vl; }
      } }
    /* SubjectPublicKeyInfo: SEQ { SEQ{alg, curve}, BITSTRING pubkey } */
    { struct der spki; if (der_enter(&tbs,0x30,&spki)) return X509_E_PARSE;
      struct der alg; if (der_enter(&spki,0x30,&alg)) return X509_E_PARSE;
      int g; const uint8_t *oc; int ol; if (der_tlv(&alg,&g,&oc,&ol)) return X509_E_PARSE;
      if (!oid_eq(oc,ol,OID_EC_PUBKEY,sizeof OID_EC_PUBKEY)) return X509_E_PARSE;
      const uint8_t *cc; int cl; if (der_tlv(&alg,&g,&cc,&cl)) return X509_E_PARSE;   /* curve OID */
      out->key_curve = oid_eq(cc,cl,OID_P256,sizeof OID_P256) ? 256 :
                       oid_eq(cc,cl,OID_P384,sizeof OID_P384) ? 384 : 0;
      const uint8_t *bs; int bl; if (der_tlv(&spki,&g,&bs,&bl)) return X509_E_PARSE;  /* BIT STRING */
      if (bl < 2 || bs[0] != 0) return X509_E_PARSE;             /* 0 unused bits */
      out->pub = bs + 1; out->publen = bl - 1;                   /* 04||X||Y */
      /* optional extensions: scan for SAN */
      if (tbs.p < tbs.end && tbs.p[0] == 0xA3) {
          struct der exts3; if (!der_enter(&tbs,0xA3,&exts3)) {
              struct der exts; if (!der_enter(&exts3,0x30,&exts)) {
                  while (exts.p < exts.end) {
                      struct der ex; if (der_enter(&exts,0x30,&ex)) break;
                      int eg; const uint8_t *eo; int eol; if (der_tlv(&ex,&eg,&eo,&eol)) break;
                      /* optional BOOLEAN critical */
                      const uint8_t *vo; int vl; int vg;
                      if (ex.p < ex.end && ex.p[0]==0x01) { if (der_tlv(&ex,&vg,&vo,&vl)) break; }
                      if (der_tlv(&ex,&vg,&vo,&vl)) break;        /* OCTET STRING value */
                      if (oid_eq(eo,eol,OID_SAN,sizeof OID_SAN)) { out->san=vo; out->sanlen=vl; }
                  }
              }
          }
      }
    }

    /* back in Certificate: signatureAlgorithm, signatureValue */
    { struct der sa; if (der_enter(&cert,0x30,&sa)) return X509_E_PARSE;
      int g; const uint8_t *oc; int ol; if (der_tlv(&sa,&g,&oc,&ol)) return X509_E_PARSE;
      out->sig_alg = oid_eq(oc,ol,OID_ECDSA_SHA256,sizeof OID_ECDSA_SHA256) ? SIG_ECDSA_SHA256 :
                     oid_eq(oc,ol,OID_ECDSA_SHA384,sizeof OID_ECDSA_SHA384) ? SIG_ECDSA_SHA384 : 0; }
    { int g; const uint8_t *bs; int bl; if (der_tlv(&cert,&g,&bs,&bl)) return X509_E_PARSE;  /* BIT STRING sig */
      if (bl < 1) return X509_E_PARSE;
      out->sig = bs + 1; out->siglen = bl - 1; }                 /* DER SEQ{r,s} */
    return X509_OK;
}

/* Convert a DER ECDSA signature SEQ{ INTEGER r, INTEGER s } into fixed r||s. */
static int sig_to_rs(const uint8_t *sig, int len, uint8_t *rs, int flen)
{
    struct der d; d.p = sig; d.end = sig + len;
    struct der seq; if (der_enter(&d, 0x30, &seq)) return -1;
    for (int half = 0; half < 2; half++) {
        int g; const uint8_t *ic; int il;
        if (der_tlv(&seq, &g, &ic, &il)) return -1;
        while (il > 0 && ic[0] == 0) { ic++; il--; }             /* strip sign byte */
        if (il > flen) return -1;
        uint8_t *dst = rs + half*flen;
        for (int i = 0; i < flen; i++) dst[i] = 0;
        for (int i = 0; i < il; i++) dst[flen - il + i] = ic[i];
    }
    return 0;
}

int x509_verify_signed_by(const struct cert *child, const struct cert *issuer)
{
    int curve = issuer->key_curve;
    if (curve != 256 && curve != 384) return X509_E_SIG;
    int flen = curve / 8;

    uint8_t hash[48]; int hlen;
    if (child->sig_alg == SIG_ECDSA_SHA256) { sha256(child->tbs, child->tbslen, hash); hlen = 32; }
    else if (child->sig_alg == SIG_ECDSA_SHA384) { sha384(child->tbs, child->tbslen, hash); hlen = 48; }
    else return X509_E_SIG;

    uint8_t rs[96];
    if (sig_to_rs(child->sig, child->siglen, rs, flen)) return X509_E_SIG;
    if (issuer->publen < 1 + 2*flen || issuer->pub[0] != 0x04) return X509_E_SIG;
    if (!ecdsa_verify(curve, issuer->pub + 1, rs, hash, hlen)) return X509_E_SIG;
    return X509_OK;
}

/* --- trusted roots (crypto/roots.c) --- */
struct root_ca { int curve; const uint8_t *pub; int publen; };  /* pub = X||Y */
extern const struct root_ca aqua_roots[];
extern const int aqua_nroots;

/* Does `issuer` match a trusted root by public key? */
static int issuer_is_trusted(const struct cert *issuer)
{
    for (int i = 0; i < aqua_nroots; i++) {
        if (aqua_roots[i].curve != issuer->key_curve) continue;
        if (issuer->publen == 1 + aqua_roots[i].publen && issuer->pub[0] == 0x04 &&
            memcmp(issuer->pub + 1, aqua_roots[i].pub, aqua_roots[i].publen) == 0)
            return 1;
    }
    return 0;
}

/* Case-insensitive host match against a pattern that may be "*.example.com". */
static int host_match(const char *host, int hl, const char *pat, int pl)
{
    if (pl >= 2 && pat[0] == '*' && pat[1] == '.') {            /* wildcard: one label */
        const char *dot = 0;
        for (int i = 0; i < hl; i++) if (host[i] == '.') { dot = host + i; break; }
        if (!dot) return 0;
        int rest = hl - (int)(dot + 1 - host);
        if (rest != pl - 2) return 0;
        for (int i = 0; i < rest; i++) {
            char a = dot[1+i], b = pat[2+i];
            if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
            if (a != b) return 0;
        }
        return 1;
    }
    if (pl != hl) return 0;
    for (int i = 0; i < hl; i++) {
        char a = host[i], b = pat[i];
        if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
        if (a != b) return 0;
    }
    return 1;
}

/* Check `host` against the leaf's SAN dNSName entries (preferred) or CN. */
static int name_ok(const struct cert *leaf, const char *host)
{
    int hl = 0; while (host[hl]) hl++;
    if (leaf->san && leaf->sanlen > 0) {
        struct der d; d.p = leaf->san; d.end = leaf->san + leaf->sanlen;
        struct der seq; if (der_enter(&d, 0x30, &seq) == 0) {
            while (seq.p < seq.end) {
                int g, l; const uint8_t *c;
                if (der_tlv(&seq, &g, &c, &l)) break;
                if (g == 0x82 /* [2] dNSName */ && host_match(host, hl, (const char *)c, l))
                    return 1;
            }
        }
        return 0;                                              /* SAN present, no match */
    }
    return leaf->cn && host_match(host, hl, leaf->cn, leaf->cnlen);
}

int x509_verify_chain(const struct cert *certs, int n, const char *host, int64_t now)
{
    if (n < 1) return X509_E_PARSE;
    /* name + validity on the leaf */
    if (!name_ok(&certs[0], host)) return X509_E_NAME;
    for (int i = 0; i < n; i++)
        if (now < certs[i].not_before || now > certs[i].not_after) return X509_E_EXPIRED;

    /* each cert signed by the next; the highest must chain to a trusted root */
    for (int i = 0; i + 1 < n; i++)
        if (x509_verify_signed_by(&certs[i], &certs[i+1]) != X509_OK) return X509_E_SIG;

    /* top of chain: trusted if it IS a root we hold, or is signed by one */
    const struct cert *top = &certs[n-1];
    if (issuer_is_trusted(top)) return X509_OK;                /* top is the trusted root */
    /* otherwise the top's issuer must be a known root that signed it: we accept
     * the chain if the top is self-consistent and its key is trusted above; if
     * not trusted, reject. */
    return X509_E_UNTRUSTED;
}
