/* TLS 1.2 client (RFC 5246, with RFC 5289 ECDHE-GCM suites, RFC 7905
 * ECDHE-ChaCha20 and RFC 7627 extended master secret).
 *
 * WHY this is a separate file rather than a branch inside tls.c: TLS 1.2 is not
 * a dialect of TLS 1.3. It has a different handshake (six messages in the
 * clear, then ChangeCipherSpec, then an encrypted Finished, instead of one
 * encrypted flight), a different key schedule (P_hash over HMAC, not an HKDF
 * ladder), and a different record layer (explicit nonces, sequence numbers in
 * the AAD, and a content type in the cleartext header instead of hidden after
 * the plaintext). What the two share -- the transport buffers, the transcript,
 * the certificate chain, the ECDHE primitives -- is in tls_int.h and tls.c.
 *
 * We enter here from tls.c the moment the ServerHello turns out to name 1.2.
 * The flow from that point is:
 *
 *      <-- Certificate, ServerKeyExchange, [CertificateRequest], ServerHelloDone
 *      --> [Certificate (empty)], ClientKeyExchange, CCS, Finished
 *      <-- [NewSessionTicket], CCS, Finished
 *
 * The security of the whole thing rests on one check that TLS 1.3 does not
 * have an equivalent of in this shape: the signature inside ServerKeyExchange.
 * See verify_ske_signature() -- it is the only thing tying the ephemeral key we
 * agree on to the certificate we verified. */

#include <stdint.h>
#include <stddef.h>
#include "tls_int.h"
#include "crypto.h"
#include "x509.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

/* TLS 1.2 SignatureAndHashAlgorithm (RFC 5246 7.4.1.4.1): hash(1) || sig(1).
 * NOTE the difference from TLS 1.3, where the same two bytes are one opaque
 * code point that also pins the curve. Here `ecdsa_sha256` says only "ECDSA
 * over SHA-256" -- which curve is whatever the certificate's key is on. Reading
 * these as 1.3 code points would reject every P-384 leaf that signs with
 * SHA-256, which is a common configuration. */
#define SIGHASH_SHA256 4
#define SIGHASH_SHA384 5
#define SIGHASH_SHA512 6
#define SIGALG_RSA     1
#define SIGALG_ECDSA   3

#define VERIFY_DATA_LEN 12               /* RFC 5246 7.4.9 for all our suites */

/* The parsed cipher suite. */
struct suite12 {
    int aead;                            /* AEAD_* */
    int keylen;                          /* AEAD key size */
    int fixed_iv;                        /* salt/IV bytes taken from the key block */
    int explicit_nonce;                  /* bytes sent in the clear per record */
    int hashlen;                         /* PRF + transcript hash */
    int need_ecdsa_cert;                 /* leaf key type the suite demands */
};

/* Decode a negotiated suite. Returns 0 on one we offered, -1 otherwise -- a
 * server that names anything else is answering an offer we did not make. */
static int suite_params(int suite, struct suite12 *o)
{
    memset(o, 0, sizeof *o);
    switch (suite) {
    case TLS_ECDHE_ECDSA_AES128_GCM_SHA256:
    case TLS_ECDHE_RSA_AES128_GCM_SHA256:
        o->aead = AEAD_AES_128_GCM; o->keylen = 16; o->fixed_iv = 4;
        o->explicit_nonce = 8; o->hashlen = 32; break;
    case TLS_ECDHE_ECDSA_AES256_GCM_SHA384:
    case TLS_ECDHE_RSA_AES256_GCM_SHA384:
        o->aead = AEAD_AES_256_GCM; o->keylen = 32; o->fixed_iv = 4;
        o->explicit_nonce = 8; o->hashlen = 48; break;
    case TLS_ECDHE_ECDSA_CHACHA20_SHA256:
    case TLS_ECDHE_RSA_CHACHA20_SHA256:
        /* RFC 7905: ChaCha20-Poly1305 in TLS 1.2 uses the TLS 1.3-style nonce
         * (a 12-byte fixed IV XORed with the sequence number) and sends NO
         * explicit nonce -- unlike the GCM suites in the same version. */
        o->aead = AEAD_CHACHA20; o->keylen = 32; o->fixed_iv = 12;
        o->explicit_nonce = 0; o->hashlen = 32; break;
    default:
        return -1;
    }
    o->need_ecdsa_cert = (suite == TLS_ECDHE_ECDSA_AES128_GCM_SHA256 ||
                          suite == TLS_ECDHE_ECDSA_AES256_GCM_SHA384 ||
                          suite == TLS_ECDHE_ECDSA_CHACHA20_SHA256);
    return 0;
}

static const char *suite_name(int suite)
{
    switch (suite) {
    case TLS_ECDHE_ECDSA_AES128_GCM_SHA256: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case TLS_ECDHE_RSA_AES128_GCM_SHA256:   return "ECDHE-RSA-AES128-GCM-SHA256";
    case TLS_ECDHE_ECDSA_AES256_GCM_SHA384: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case TLS_ECDHE_RSA_AES256_GCM_SHA384:   return "ECDHE-RSA-AES256-GCM-SHA384";
    case TLS_ECDHE_ECDSA_CHACHA20_SHA256:   return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case TLS_ECDHE_RSA_CHACHA20_SHA256:     return "ECDHE-RSA-CHACHA20-POLY1305";
    default:                                return "?";
    }
}

/* ------------------------------------------------------------ record layer */

/* Build the TLS 1.2 AEAD additional data (RFC 5246 6.2.3.3):
 *      seq_num(8) || type(1) || version(2) || length(2)
 * `length` is the PLAINTEXT length, not the record's. The sequence number is
 * never transmitted -- it is implicit state on both sides, and including it
 * here is what stops an attacker from reordering, replaying or deleting
 * records: any of those makes the tag fail. */
static void aad12(uint64_t seq, uint8_t type, int ptlen, uint8_t out[13])
{
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(seq >> (56 - 8 * i));
    out[8] = type;
    out[9] = 0x03; out[10] = 0x03;
    out[11] = (uint8_t)(ptlen >> 8); out[12] = (uint8_t)ptlen;
}

/* Assemble the record nonce for direction `a` and explicit part `expl`.
 *   GCM (RFC 5288):  salt(4) || explicit(8), and the explicit half travels in
 *                    the clear at the head of the record.
 *   ChaCha20 (RFC 7905): the full 12-byte IV XORed with the sequence number,
 *                    with nothing on the wire.
 * Either way the nonce is unique per record under a key, which is the one
 * thing GCM and Poly1305 both catastrophically require. */
static void nonce12(const struct aead *a, const uint8_t *expl, uint8_t nonce[12])
{
    if (a->explicit_nonce) {
        memcpy(nonce, a->iv, 4);
        memcpy(nonce + 4, expl, 8);
    } else {
        memcpy(nonce, a->iv, 12);
        for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(a->seq >> (8 * i));
    }
}

int tls12_write_record(struct tls_sess *s, uint8_t type, const uint8_t *pt, int len)
{
    if (len < 0 || len > SEND_REC_MAX) return -1;
    if (!s->tx_encrypted) return tls_tx_queue(s, type, pt, len);

    struct aead *a = &s->cw;
    /* Sized for the largest record we ever emit: explicit nonce + plaintext +
     * tag. static rather than automatic to keep 4 KiB off the kernel stack, for
     * the same reason the 1.3 seal path does it -- and with the same safety
     * argument: sealing never yields, it runs start to finish inside one call,
     * so concurrent sessions cannot interleave here. */
    static uint8_t rec[8 + SEND_REC_MAX + 16];
    int n = 0;
    uint8_t expl[8];
    /* The explicit nonce is the sequence number. RFC 5288 only requires it to
     * be unique per key; using the counter we already maintain makes that true
     * by construction, and needs no extra randomness. */
    for (int i = 0; i < 8; i++) expl[i] = (uint8_t)(a->seq >> (56 - 8 * i));
    if (a->explicit_nonce) { memcpy(rec, expl, 8); n = 8; }

    uint8_t nonce[12]; nonce12(a, expl, nonce);
    uint8_t aad[13]; aad12(a->seq, type, len, aad);
    tls_aead_encrypt(a, nonce, aad, 13, pt, len, rec + n, rec + n + len);
    a->seq++;
    return tls_tx_queue(s, type, rec, n + len + 16);
}

int tls12_read_record(struct tls_sess *s, uint8_t *ctype, int *len)
{
    int r = tls_rec_pull(s);
    if (r <= 0) return r;

    const uint8_t *body = s->rxrec + 5;
    int blen = s->reclen;
    *ctype = s->rectype;

    if (!s->rx_encrypted) {
        if (blen > (int)sizeof s->app) { tls_rec_drop(s); return -1; }
        memcpy(s->app, body, (size_t)blen);
        *len = blen;
        tls_rec_drop(s);
        return 1;
    }

    struct aead *a = &s->cr;
    int overhead = a->explicit_nonce + 16;
    if (blen < overhead) { tls_rec_drop(s); return -1; }
    int ptlen = blen - overhead;
    if (ptlen > (int)sizeof s->app) { tls_rec_drop(s); return -1; }

    uint8_t nonce[12]; nonce12(a, body, nonce);
    uint8_t aad[13]; aad12(a->seq, s->rectype, ptlen, aad);
    const uint8_t *ct = body + a->explicit_nonce;
    int rc = tls_aead_decrypt(a, nonce, aad, 13, ct, ptlen, ct + ptlen, s->app);
    tls_rec_drop(s);
    if (rc) return -1;
    a->seq++;
    *len = ptlen;
    return 1;
}

/* ------------------------------------------------------------- key schedule */

/* Install the key block into the two directions (RFC 5246 6.3). The block is
 *      client_write_key || server_write_key || client_write_IV || server_write_IV
 * with no MAC keys, because every suite we offer is AEAD. Note the PRF seed is
 * server_random || client_random here -- the OPPOSITE order from the master
 * secret's. Swapping them yields a key block both sides compute differently,
 * which surfaces only as a Finished mismatch. */
static void install_keys(struct tls_sess *s, const struct suite12 *sp)
{
    uint8_t seed[64];
    memcpy(seed, s->srandom, 32);
    memcpy(seed + 32, s->random, 32);

    uint8_t block[2 * 32 + 2 * 12];
    int need = 2 * sp->keylen + 2 * sp->fixed_iv;
    tls12_prf(sp->hashlen, s->master, 48, "key expansion", seed, 64, block, need);

    int p = 0;
    s->cw.alg = s->cr.alg = sp->aead;
    s->cw.keylen = s->cr.keylen = sp->keylen;
    s->cw.ivlen = s->cr.ivlen = sp->fixed_iv;
    s->cw.explicit_nonce = s->cr.explicit_nonce = sp->explicit_nonce;
    memcpy(s->cw.key, block + p, (size_t)sp->keylen); p += sp->keylen;
    memcpy(s->cr.key, block + p, (size_t)sp->keylen); p += sp->keylen;
    memcpy(s->cw.iv,  block + p, (size_t)sp->fixed_iv); p += sp->fixed_iv;
    memcpy(s->cr.iv,  block + p, (size_t)sp->fixed_iv);
    s->cw.seq = s->cr.seq = 0;
    crypto_wipe(block, sizeof block);
}

/* Derive the master secret from the premaster (RFC 5246 8.1, RFC 7627 4).
 *
 * Two constructions, and which one runs is a security decision:
 *   - Extended master secret: PRF(pm, "extended master secret", session_hash),
 *     where session_hash covers every handshake message through
 *     ClientKeyExchange. This binds the master secret to the whole handshake --
 *     including the certificate and the ServerKeyExchange -- so the same
 *     premaster cannot be made to yield the same master secret in two different
 *     handshakes. That is what defeats the triple-handshake attack.
 *   - Plain: PRF(pm, "master secret", client_random || server_random). This is
 *     what a server that did not echo the extension gets.
 *
 * We ASK for EMS on every connection and use it whenever the server echoes it.
 * When the server does not, we continue with the plain derivation rather than
 * refusing: the attack it prevents needs the client to authenticate to two
 * servers with the same credential across the spliced handshakes, and this
 * client never sends a client certificate and never resumes -- so it cannot be
 * the victim of it. Refusing instead would cost real reachability (older
 * 1.2-only appliances, which are precisely the servers we came here for) to
 * close a hole we are not standing in front of. The choice is logged. */
static void derive_master(struct tls_sess *s, const struct suite12 *sp,
                          const uint8_t *pm, int pmlen)
{
    if (s->ems) {
        uint8_t session_hash[48];
        tls_th_hash(s, session_hash);
        tls12_prf(sp->hashlen, pm, pmlen, "extended master secret",
                  session_hash, sp->hashlen, s->master, 48);
        crypto_wipe(session_hash, sizeof session_hash);
    } else {
        uint8_t seed[64];
        memcpy(seed, s->random, 32);
        memcpy(seed + 32, s->srandom, 32);
        tls12_prf(sp->hashlen, pm, pmlen, "master secret", seed, 64, s->master, 48);
    }
}

/* verify_data for a Finished message (RFC 5246 7.4.9):
 *      PRF(master_secret, finished_label, Hash(handshake_messages))[0..11] */
static void finished_verify_data(struct tls_sess *s, const struct suite12 *sp,
                                 const char *label, uint8_t out[VERIFY_DATA_LEN])
{
    uint8_t th[48];
    tls_th_hash(s, th);
    tls12_prf(sp->hashlen, s->master, 48, label, th, sp->hashlen, out, VERIFY_DATA_LEN);
    crypto_wipe(th, sizeof th);
}

/* -------------------------------------------------- ServerKeyExchange check */

/* Verify the signature over the server's ephemeral key parameters.
 *
 * THIS IS THE CHECK THAT MAKES TLS 1.2 ECDHE AUTHENTICATED. The certificate
 * chain proves who owns the long-term key; the ECDHE share is freshly
 * generated and proves nothing on its own. The only thing binding the two is
 * this signature, made by the certificate's key over
 *
 *      client_random || server_random || ServerECDHParams
 *
 * Skip it -- or accept it without checking which key made it -- and any
 * on-path attacker can substitute their own ECDHE share, agree a key with us,
 * and forward a genuine certificate they do not hold the private key for. The
 * handshake would complete, every subsequent record would decrypt, and nothing
 * else in the protocol would ever notice. The two randoms are in the signed
 * blob for a second reason: they make the signature specific to THIS handshake,
 * so one recorded ServerKeyExchange cannot be replayed into another.
 *
 * `signed_data` is the blob above; `cert` is the verified leaf. Returns 1 if
 * the signature is good, 0 otherwise. */
static int verify_ske_signature(const struct cert *leaf, int sighash, int sigalg,
                                const uint8_t *signed_data, int sdlen,
                                const uint8_t *sig, int siglen)
{
    int hlen = sighash == SIGHASH_SHA256 ? 32
             : sighash == SIGHASH_SHA384 ? 48
             : sighash == SIGHASH_SHA512 ? 64 : 0;
    if (!hlen) return 0;

    uint8_t hash[64];
    if (hlen == 32)      sha256(signed_data, (size_t)sdlen, hash);
    else if (hlen == 48) sha384(signed_data, (size_t)sdlen, hash);
    else                 sha512(signed_data, (size_t)sdlen, hash);

    if (sigalg == SIGALG_ECDSA) {
        /* The curve comes from the CERTIFICATE, not from the code point -- see
         * the note on SignatureAndHashAlgorithm above. A P-384 leaf signing
         * with SHA-256 is legal and common. */
        if (leaf->key_type != KEY_EC) return 0;
        int curve = leaf->key_curve;
        if (curve != 256 && curve != 384) return 0;
        int flen = curve / 8;
        if (leaf->publen != 1 + 2 * flen || leaf->pub[0] != 0x04) return 0;
        uint8_t rs[96];
        if (x509_der_sig_to_rs(sig, siglen, rs, flen) != 0) return 0;
        return ecdsa_verify(curve, leaf->pub + 1, rs, hash, hlen);
    }
    if (sigalg == SIGALG_RSA) {
        if (leaf->key_type != KEY_RSA) return 0;
        return rsa_pkcs1_verify(leaf->rsa_n, leaf->rsa_nlen, leaf->rsa_e, leaf->rsa_elen,
                                sig, siglen, hash, hlen);
    }
    /* RFC 8446 4.2.3 also allows rsa_pss_rsae_* (0x08xx) in a TLS 1.2
     * ServerKeyExchange when the client advertised them, and we do. Those code
     * points do not decompose into (hash, sigalg) the way the legacy ones do,
     * so they are handled by the caller before we get here. */
    return 0;
}

/* --------------------------------------------------------- the handshake */

/* Does the buffered flight end with ServerHelloDone? */
static int flight12_complete(const struct tls_sess *s)
{
    int q = 0;
    while (q + 4 <= s->hslen) {
        int ml = (s->hsbuf[q+1] << 16) | (s->hsbuf[q+2] << 8) | s->hsbuf[q+3];
        if (q + 4 + ml > s->hslen) return 0;
        if (s->hsbuf[q] == HS_SERVER_DONE) return 1;
        q += 4 + ml;
    }
    return 0;
}

/* Process Certificate .. ServerHelloDone, then build and queue our whole second
 * flight. Returns 0 on success or a negative TLS_E_*. */
static int process_flight(struct tls_sess *s)
{
    struct suite12 sp;
    if (suite_params(s->suite, &sp) != 0) {
        kprintf("[tls] server chose suite 0x%x, which we did not offer\n", s->suite);
        return TLS_E_PROTO;
    }

    /* static, not automatic: 8 parsed certificates are ~1 KiB the kernel stack
     * should not carry. Safe with several live sessions because process_flight
     * runs start to finish inside one tls_step() call and never yields. */
    static struct cert chain[8];
    int ncert = 0;
    const uint8_t *ske = 0; int skelen = 0;
    const uint8_t *flight = s->hsbuf;
    int flen = s->hslen, q = 0;

    while (q + 4 <= flen) {
        int mt = flight[q];
        int ml = (flight[q+1] << 16) | (flight[q+2] << 8) | flight[q+3];
        if (q + 4 + ml > flen) break;                    /* ignore an incomplete tail */
        const uint8_t *mb = flight + q + 4;

        if (mt == HS_CERTIFICATE) {
            /* TLS 1.2's Certificate is simpler than 1.3's: a 3-byte list length
             * then {3-byte length, DER} with no per-certificate extensions and
             * no request context. Every length is bounded by ml, so a crafted
             * message cannot read past the handshake body. */
            if (ml < 3) return TLS_E_PROTO;
            int listlen = (mb[0] << 16) | (mb[1] << 8) | mb[2];
            int cp = 3, cend = 3 + listlen;
            if (cend > ml) return TLS_E_PROTO;
            while (cp + 3 <= cend && ncert < 8) {
                int clen = (mb[cp] << 16) | (mb[cp+1] << 8) | mb[cp+2]; cp += 3;
                if (clen < 0 || cp + clen > cend) return TLS_E_PROTO;
                if (x509_parse(mb + cp, clen, &chain[ncert]) == 0) ncert++;
                cp += clen;
            }
        } else if (mt == HS_SERVER_KX) {
            ske = mb; skelen = ml;
        } else if (mt == HS_CERT_REQUEST) {
            /* We hold no client certificate. RFC 5246 7.4.6 says to answer with
             * an EMPTY Certificate message rather than saying nothing -- a
             * client that just omits it is a protocol error and most servers
             * will drop the connection. Declining politely is what lets a
             * server configured for optional client auth still serve us. */
            s->cert_req = 1;
        }
        q += 4 + ml;
    }

    /* --- the certificate chain --- */
    int cr = tls_check_chain(s, chain, ncert);
    if (cr) return cr;
    if (sp.need_ecdsa_cert && chain[0].key_type != KEY_EC) {
        /* ECDHE_ECDSA with an RSA leaf (or vice versa) is a server contradicting
         * its own suite selection. Nothing good comes of guessing. */
        kprintf("[tls] suite 0x%x needs an ECDSA leaf, got key type %d\n",
                s->suite, chain[0].key_type);
        return TLS_E_CERT;
    }
    if (!sp.need_ecdsa_cert && chain[0].key_type != KEY_RSA) {
        kprintf("[tls] suite 0x%x needs an RSA leaf, got key type %d\n",
                s->suite, chain[0].key_type);
        return TLS_E_CERT;
    }

    /* --- ServerKeyExchange ---
     *   ECParameters: curve_type(1) must be named_curve(3), namedcurve(2)
     *   ECPoint:      length(1) || point
     *   then          SignatureAndHashAlgorithm(2) || signature<2>
     * ECDHE is the only key exchange we offer, so a suite that reached this
     * point without a ServerKeyExchange is a server that skipped the step that
     * authenticates its ephemeral key. */
    if (!ske) { kprintf("[tls] no ServerKeyExchange in a 1.2 ECDHE handshake\n"); return TLS_E_PROTO; }
    if (skelen < 4) return TLS_E_PROTO;
    if (ske[0] != 3) {                                   /* named_curve */
        kprintf("[tls] ServerKeyExchange curve_type %d (not named_curve)\n", ske[0]);
        return TLS_E_PROTO;
    }
    int grp = (ske[1] << 8) | ske[2];
    int plen = ske[3];
    int params_len = 4 + plen;                           /* the signed ECParameters+point */
    /* Every one of these is a length the SERVER chose, so each is checked
     * against what actually arrived before it is used to index anything. The
     * log line matters: without it a malformed message and a wrong-curve
     * message are the same silent TLS_E_PROTO. */
    if (plen < 1 || plen > (int)sizeof s->speer || params_len + 4 > skelen) {
        kprintf("[tls] ServerKeyExchange malformed: point len %d in a %d-byte message\n",
                plen, skelen);
        return TLS_E_PROTO;
    }
    if (!tls_group_supported(grp)) {
        kprintf("[tls] ServerKeyExchange named curve 0x%x is not one we offered\n", grp);
        return TLS_E_PROTO;
    }

    int sp_off = params_len;
    int sighash = ske[sp_off], sigalg = ske[sp_off + 1];
    int siglen = (ske[sp_off + 2] << 8) | ske[sp_off + 3];
    if (siglen < 1 || sp_off + 4 + siglen > skelen) {
        kprintf("[tls] ServerKeyExchange malformed: signature len %d in a %d-byte message\n",
                siglen, skelen);
        return TLS_E_PROTO;
    }
    const uint8_t *sig = ske + sp_off + 4;

    /* The signed blob: client_random || server_random || ServerECDHParams. */
    uint8_t signed_data[32 + 32 + 4 + 97]; int sd = 0;
    memcpy(signed_data + sd, s->random, 32);  sd += 32;
    memcpy(signed_data + sd, s->srandom, 32); sd += 32;
    memcpy(signed_data + sd, ske, (size_t)params_len); sd += params_len;

    int okv = 0;
    if (sighash == 0x08) {
        /* rsa_pss_rsae_sha256/384/512: in TLS 1.2's two-byte encoding these
         * appear as 0x08 0x04/05/06, where the first byte is not a hash id at
         * all. Handled here rather than in verify_ske_signature so the legacy
         * decomposition there stays honest about what it means. */
        int hh = sigalg == 0x04 ? 32 : sigalg == 0x05 ? 48 : sigalg == 0x06 ? 64 : 0;
        uint8_t hash[64];
        if (hh && chain[0].key_type == KEY_RSA) {
            if (hh == 32)      sha256(signed_data, (size_t)sd, hash);
            else if (hh == 48) sha384(signed_data, (size_t)sd, hash);
            else               sha512(signed_data, (size_t)sd, hash);
            okv = rsa_pss_verify(chain[0].rsa_n, chain[0].rsa_nlen,
                                 chain[0].rsa_e, chain[0].rsa_elen, sig, siglen, hash, hh);
        }
    } else {
        okv = verify_ske_signature(&chain[0], sighash, sigalg, signed_data, sd, sig, siglen);
    }
    if (!okv) {
        kprintf("[tls] ServerKeyExchange signature rejected (hash %d alg %d, %d bytes)\n",
                sighash, sigalg, siglen);
        return TLS_E_CERT;
    }
    kprintf("[tls] ServerKeyExchange: group %s, signature verified (hash %d alg %d)\n",
            tls_group_name(grp), sighash, sigalg);

    /* --- our ephemeral key, on the curve the server named --- */
    memcpy(s->speer, ske + 4, (size_t)plen);
    s->speerlen = plen;
    s->group = grp;
    crypto_wipe(s->priv, sizeof s->priv);
    if (tls_gen_share(s) != 0) return TLS_E_CRYPTO;

    uint8_t pm[48]; int pmlen = 0;
    if (tls_compute_shared(s, s->speer, s->speerlen, pm, &pmlen) != 0) {
        crypto_wipe(pm, sizeof pm);
        return TLS_E_CRYPTO;
    }

    /* --- our flight ---
     * The transcript must be extended in wire order, and the master secret must
     * be derived AFTER ClientKeyExchange goes into it, because RFC 7627's
     * session_hash is defined as the hash up to and including that message. */
    uint8_t out[16 + 8 + 200]; int n = 0;

    if (s->cert_req) {                                   /* empty Certificate */
        out[n++] = HS_CERTIFICATE; out[n++] = 0; out[n++] = 0; out[n++] = 3;
        out[n++] = 0; out[n++] = 0; out[n++] = 0;        /* zero-length list */
    }
    out[n++] = HS_CLIENT_KX;
    out[n++] = 0; out[n++] = 0; out[n++] = (uint8_t)(1 + s->publen);
    out[n++] = (uint8_t)s->publen;
    memcpy(out + n, s->pub, (size_t)s->publen); n += s->publen;
    tls_th_update(s, out, n);

    derive_master(s, &sp, pm, pmlen);
    crypto_wipe(pm, sizeof pm);
    install_keys(s, &sp);

    if (tls_tx_queue(s, REC_HANDSHAKE, out, n) != 0) return TLS_E_PROTO;

    /* ChangeCipherSpec. In TLS 1.2 this is a real protocol message on its own
     * content type, and it is the thing that switches the write direction to
     * the keys we just installed -- unlike 1.3, where a CCS on the wire is a
     * meaningless byte kept only to fool middleboxes. It is NOT part of the
     * handshake transcript. */
    uint8_t ccs = 1;
    if (tls_tx_queue(s, REC_CCS, &ccs, 1) != 0) return TLS_E_PROTO;
    s->tx_encrypted = 1;

    uint8_t fin[4 + VERIFY_DATA_LEN];
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = VERIFY_DATA_LEN;
    finished_verify_data(s, &sp, "client finished", fin + 4);
    tls_th_update(s, fin, (int)sizeof fin);              /* the server hashes it too */
    if (tls12_write_record(s, REC_HANDSHAKE, fin, (int)sizeof fin) != 0) return TLS_E_PROTO;
    /* The server's flight is fully consumed; hsbuf now belongs to the messages
     * arriving after the CCS. */
    s->hslen = 0;
    return 0;
}

/* Read the server's CCS and its encrypted Finished, and check it.
 *
 * Messages are re-buffered in s->hsbuf rather than parsed straight out of the
 * record: TLS record boundaries and handshake message boundaries are unrelated,
 * so one record can hold several messages AND one message can be split across
 * records. A NewSessionTicket ahead of the Finished is the case that makes this
 * concrete on real servers. */
static int step12_recv_ccs(struct tls_sess *s)
{
    struct suite12 sp;
    if (suite_params(s->suite, &sp) != 0) return tls_fail(s, TLS_E_PROTO);

    for (;;) {
        /* Consume every complete message we already hold. */
        int q = 0;
        while (q + 4 <= s->hslen) {
            int mt = s->hsbuf[q];
            int ml = (s->hsbuf[q+1] << 16) | (s->hsbuf[q+2] << 8) | s->hsbuf[q+3];
            if (q + 4 + ml > s->hslen) break;         /* wait for the rest */

            if (mt == HS_FINISHED) {
                if (!s->rx_encrypted) {
                    /* A Finished before the server's CCS would be an
                     * unencrypted one -- i.e. an attacker's, not the server's. */
                    kprintf("[tls] server Finished arrived before its CCS\n");
                    return tls_fail(s, TLS_E_PROTO);
                }
                uint8_t expect[VERIFY_DATA_LEN];
                finished_verify_data(s, &sp, "server finished", expect);
                /* This MAC covers every handshake byte in both directions. It
                 * is what proves the server derived the same master secret --
                 * and, because the transcript includes our ClientHello, that
                 * nobody edited our version or suite list in flight. */
                int bad = (ml != VERIFY_DATA_LEN) ||
                          memcmp(expect, s->hsbuf + q + 4, VERIFY_DATA_LEN) != 0;
                crypto_wipe(expect, sizeof expect);
                if (bad) {
                    kprintf("[tls] server Finished verify_data mismatch\n");
                    return tls_fail(s, TLS_E_CRYPTO);
                }
                crypto_wipe(s->master, sizeof s->master);
                crypto_wipe(s->priv, sizeof s->priv);
                s->hslen = 0;
                s->state = TS_ESTABLISHED;
                return TLS_DONE;
            }
            /* Anything else here (NewSessionTicket) is still part of the
             * transcript the server's Finished covers, so it must be hashed
             * even though we do nothing with it -- we do not resume. */
            tls_th_update(s, s->hsbuf + q, 4 + ml);
            q += 4 + ml;
        }
        if (q) { memmove(s->hsbuf, s->hsbuf + q, (size_t)(s->hslen - q)); s->hslen -= q; }

        uint8_t ct; int dl;
        int r = tls12_read_record(s, &ct, &dl);
        if (r == 0) return TLS_WANT_READ;
        if (r < 0) return tls_fail(s, TLS_E_TCP);

        if (ct == REC_ALERT) { tls_log_alert(s->app, dl); return tls_fail(s, TLS_E_PROTO); }
        if (ct == REC_CCS) {
            if (dl != 1 || s->app[0] != 1) return tls_fail(s, TLS_E_PROTO);
            /* The read direction switches here, and its sequence number
             * restarts at zero -- install_keys already set it, and nothing has
             * decrypted since. A CCS is NOT hashed into the transcript. */
            s->rx_encrypted = 1;
            s->cr.seq = 0;
            continue;
        }
        if (ct != REC_HANDSHAKE) return tls_fail(s, TLS_E_PROTO);
        if (s->hslen + dl > (int)sizeof s->hsbuf) return tls_fail(s, TLS_E_PROTO);
        memcpy(s->hsbuf + s->hslen, s->app, (size_t)dl);
        s->hslen += dl;
    }
}

/* Collect the server's first flight. */
static int step12_recv_flight(struct tls_sess *s)
{
    while (!flight12_complete(s)) {
        uint8_t ct; int dl;
        int r = tls12_read_record(s, &ct, &dl);
        if (r == 0) return TLS_WANT_READ;
        if (r < 0) return tls_fail(s, TLS_E_TCP);
        if (ct == REC_ALERT) { tls_log_alert(s->app, dl); return tls_fail(s, TLS_E_PROTO); }
        if (ct != REC_HANDSHAKE) return tls_fail(s, TLS_E_PROTO);
        if (s->hslen + dl > (int)sizeof s->hsbuf) {
            kprintf("[tls] 1.2 server flight exceeds %d bytes\n", (int)sizeof s->hsbuf);
            return tls_fail(s, TLS_E_PROTO);
        }
        /* Hash as we go: these bytes are the transcript, in the order they
         * arrived, and the two Finished MACs are computed over exactly them. */
        tls_th_update(s, s->app, dl);
        memcpy(s->hsbuf + s->hslen, s->app, (size_t)dl);
        s->hslen += dl;
    }

    int rc = process_flight(s);
    if (rc) return tls_fail(s, rc);

    s->state = TS12_RECV_CCS;
    int fl = tls_tx_flush(s);
    if (fl < 0) return tls_fail(s, TLS_E_TCP);
    if (!fl) return TLS_WANT_WRITE;
    return step12_recv_ccs(s);
}

int tls12_begin(struct tls_sess *s)
{
    struct suite12 sp;
    if (suite_params(s->suite, &sp) != 0) {
        kprintf("[tls] TLS 1.2 server chose suite 0x%x, which we did not offer\n", s->suite);
        return tls_fail(s, TLS_E_PROTO);
    }
    /* From here the transcript hash is the suite's, not SHA-256 by default.
     * tls_th_update has been feeding both hashes since the ClientHello, so
     * switching now is just a choice of which running state to read. */
    s->hashlen = sp.hashlen;
    /* Announce the negotiation here rather than at the end of the handshake, so
     * the serial log reads in the order the protocol happened: ServerHello,
     * then the chain, then the ServerKeyExchange signature. The group is not
     * known yet -- in 1.2 the server names it in the ServerKeyExchange, which
     * has not arrived. */
    kprintf("[tls] ServerHello: TLS 1.2, suite 0x%x %s, ems=%s\n",
            s->suite, suite_name(s->suite), s->ems ? "yes" : "no");
    s->state = TS12_RECV_FLIGHT;
    /* The Certificate almost always shares a TCP segment with the ServerHello,
     * so carry straight on rather than reporting WANT_READ for bytes we hold. */
    return step12_recv_flight(s);
}

int tls12_step(struct tls_sess *s)
{
    switch (s->state) {
    case TS12_RECV_FLIGHT: return step12_recv_flight(s);
    case TS12_RECV_CCS:    return step12_recv_ccs(s);
    default:               return tls_fail(s, TLS_E_PROTO);
    }
}
