#include <stdint.h>
#include <stddef.h>
#include "x509.h"
#include "crypto.h"
#include "roots.h"

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
static const uint8_t OID_RSA_SHA256[]   = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const uint8_t OID_RSA_SHA384[]   = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
static const uint8_t OID_EC_PUBKEY[]    = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const uint8_t OID_RSA_ENC[]      = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
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
    /* SubjectPublicKeyInfo: SEQ { AlgorithmIdentifier, BITSTRING pubkey } */
    out->key_type = 0; out->key_curve = 0;
    out->pub = 0; out->publen = 0; out->rsa_n = 0; out->rsa_nlen = 0; out->rsa_e = 0; out->rsa_elen = 0;
    { struct der spki; if (der_enter(&tbs,0x30,&spki)) return X509_E_PARSE;
      struct der alg; if (der_enter(&spki,0x30,&alg)) return X509_E_PARSE;
      int g; const uint8_t *oc; int ol; if (der_tlv(&alg,&g,&oc,&ol)) return X509_E_PARSE;
      if (oid_eq(oc,ol,OID_EC_PUBKEY,sizeof OID_EC_PUBKEY)) {
          const uint8_t *cc; int cl; if (der_tlv(&alg,&g,&cc,&cl)) return X509_E_PARSE;   /* curve OID */
          out->key_type = KEY_EC;
          out->key_curve = oid_eq(cc,cl,OID_P256,sizeof OID_P256) ? 256 :
                           oid_eq(cc,cl,OID_P384,sizeof OID_P384) ? 384 : 0;
          const uint8_t *bs; int bl; if (der_tlv(&spki,&g,&bs,&bl)) return X509_E_PARSE; /* BIT STRING */
          if (bl < 2 || bs[0] != 0) return X509_E_PARSE;         /* 0 unused bits */
          out->pub = bs + 1; out->publen = bl - 1;               /* 04||X||Y */
      } else if (oid_eq(oc,ol,OID_RSA_ENC,sizeof OID_RSA_ENC)) {
          out->key_type = KEY_RSA;
          const uint8_t *bs; int bl; if (der_tlv(&spki,&g,&bs,&bl)) return X509_E_PARSE; /* BIT STRING */
          if (bl < 2 || bs[0] != 0) return X509_E_PARSE;         /* 0 unused bits */
          struct der pk; pk.p = bs + 1; pk.end = bs + bl;        /* RSAPublicKey */
          struct der rsa; if (der_enter(&pk,0x30,&rsa)) return X509_E_PARSE;
          const uint8_t *nc; int nl; if (der_tlv(&rsa,&g,&nc,&nl)) return X509_E_PARSE;  /* modulus */
          while (nl > 0 && nc[0] == 0) { nc++; nl--; }
          const uint8_t *ec; int el; if (der_tlv(&rsa,&g,&ec,&el)) return X509_E_PARSE;  /* exponent */
          while (el > 0 && ec[0] == 0) { ec++; el--; }
          out->rsa_n = nc; out->rsa_nlen = nl; out->rsa_e = ec; out->rsa_elen = el;
      } else return X509_E_PARSE;
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
                     oid_eq(oc,ol,OID_ECDSA_SHA384,sizeof OID_ECDSA_SHA384) ? SIG_ECDSA_SHA384 :
                     oid_eq(oc,ol,OID_RSA_SHA256,  sizeof OID_RSA_SHA256)   ? SIG_RSA_SHA256   :
                     oid_eq(oc,ol,OID_RSA_SHA384,  sizeof OID_RSA_SHA384)   ? SIG_RSA_SHA384   : 0; }
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

/* Hash `child`'s tbs per its signature algorithm. Returns hlen (32/48) or 0. */
static int hash_tbs(const struct cert *child, uint8_t hash[48])
{
    switch (child->sig_alg) {
    case SIG_ECDSA_SHA256: case SIG_RSA_SHA256: sha256(child->tbs, child->tbslen, hash); return 32;
    case SIG_ECDSA_SHA384: case SIG_RSA_SHA384: sha384(child->tbs, child->tbslen, hash); return 48;
    default: return 0;
    }
}

/* Verify `child`'s signature with `issuer`'s key (EC point or RSA n,e). */
static int verify_with_key(const struct cert *child, int issuer_type, int issuer_curve,
                           const uint8_t *ec, int eclen,
                           const uint8_t *n, int nlen, const uint8_t *e, int elen)
{
    uint8_t hash[48]; int hlen = hash_tbs(child, hash);
    if (!hlen) return 0;
    int rsa = (child->sig_alg == SIG_RSA_SHA256 || child->sig_alg == SIG_RSA_SHA384);
    if (rsa) {
        if (issuer_type != KEY_RSA) return 0;
        return rsa_pkcs1_verify(n, nlen, e, elen, child->sig, child->siglen, hash, hlen);
    }
    if (issuer_type != KEY_EC || (issuer_curve != 256 && issuer_curve != 384)) return 0;
    int flen = issuer_curve / 8;
    uint8_t rs[96];
    if (sig_to_rs(child->sig, child->siglen, rs, flen)) return 0;
    if (eclen < 2*flen) return 0;
    return ecdsa_verify(issuer_curve, ec, rs, hash, hlen);
}

int x509_verify_signed_by(const struct cert *child, const struct cert *issuer)
{
    const uint8_t *ec = (issuer->publen >= 1 && issuer->pub && issuer->pub[0] == 0x04)
                        ? issuer->pub + 1 : issuer->pub;
    int eclen = issuer->publen > 0 ? issuer->publen - 1 : 0;
    return verify_with_key(child, issuer->key_type, issuer->key_curve, ec, eclen,
                           issuer->rsa_n, issuer->rsa_nlen, issuer->rsa_e, issuer->rsa_elen)
           ? X509_OK : X509_E_SIG;
}

/* --- trusted roots (crypto/roots.c, generated by tools/genroots.py) --- */

/* `c`'s own public key is byte-identical to a held root (server sent the root
 * in-band; trust it directly without re-checking its self-signature). */
static int is_pinned_root(const struct cert *c)
{
    for (int i = 0; i < aqua_nroots; i++) {
        const struct root_ca *r = &aqua_roots[i];
        if (r->type == ROOT_EC && c->key_type == KEY_EC && r->curve == c->key_curve) {
            if (c->publen == 1 + r->eclen && c->pub[0] == 0x04 &&
                memcmp(c->pub + 1, r->ec, r->eclen) == 0) return 1;
        } else if (r->type == ROOT_RSA && c->key_type == KEY_RSA) {
            if (c->rsa_nlen == r->nlen && memcmp(c->rsa_n, r->n, r->nlen) == 0 &&
                c->rsa_elen == r->elen && memcmp(c->rsa_e, r->e, r->elen) == 0) return 1;
        }
    }
    return 0;
}

/* `top`'s issuer is one of our roots: its signature verifies under a held root
 * key (the common case -- servers send leaf+intermediates, not the root). */
static int signed_by_root(const struct cert *top)
{
    int rsa = (top->sig_alg == SIG_RSA_SHA256 || top->sig_alg == SIG_RSA_SHA384);
    for (int i = 0; i < aqua_nroots; i++) {
        const struct root_ca *r = &aqua_roots[i];
        if (rsa) {
            if (r->type != ROOT_RSA) continue;
            if (verify_with_key(top, KEY_RSA, 0, 0, 0, r->n, r->nlen, r->e, r->elen)) return 1;
        } else {
            if (r->type != ROOT_EC) continue;
            if (verify_with_key(top, KEY_EC, r->curve, r->ec, r->eclen, 0, 0, 0, 0)) return 1;
        }
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

    /* top of chain: trusted if it IS a root we hold (sent in-band), or if its
     * issuer is one of our roots (i.e. a held root key signed it). */
    const struct cert *top = &certs[n-1];
    if (is_pinned_root(top)) return X509_OK;
    if (signed_by_root(top)) return X509_OK;
    return X509_E_UNTRUSTED;
}
