#ifndef LOGIT_TLS_INT_H
#define LOGIT_TLS_INT_H

/* Shared internals of the TLS client. Not a public interface -- tls.h is.
 *
 * This exists because TLS 1.2 and TLS 1.3 are two different protocols that
 * happen to share a socket, a certificate chain and a set of primitives. They
 * share almost nothing else: different handshakes, different key schedules,
 * different record layers. Folding 1.2 into tls.c as a set of `if (version ==
 * ...)` branches would have made both harder to read and the 1.3 path -- the
 * one that works today -- easier to break. So the session, the transport
 * buffers and the transcript live here; tls.c owns version negotiation, the 1.3
 * handshake and the public API; tls12.c owns the 1.2 handshake and its record
 * layer. */

#include <stdint.h>
#include "crypto.h"
#include "tls.h"
#include "x509.h"
#include "tls_prof.h"

/* --- cipher suites --------------------------------------------------------
 * TLS 1.3 (RFC 8446): the AEAD and the hash, nothing else -- key exchange and
 * authentication are negotiated separately. */
#define TLS_AES_128_GCM_SHA256       0x1301
#define TLS_AES_256_GCM_SHA384       0x1302
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

/* The transcript/HKDF hash a TLS 1.3 suite names, in bytes. This is the ONE
 * place the mapping lives, because 0x1302 is the first 1.3 suite here that is
 * not SHA-256 and the whole risk of adding it is a 32 written somewhere it
 * should have been derived. A key length, a Finished length, an
 * HKDF-Expand-Label output width or a transcript hash left at 32 does not fail
 * where it is wrong -- it fails at the server's Finished MAC, which points at
 * the key schedule in general and at nothing in particular. */
static inline int tls13_suite_hash(int suite)
{ return suite == TLS_AES_256_GCM_SHA384 ? 48 : 32; }

/* TLS 1.2 (RFC 5289 / RFC 7905). ECDHE only, AEAD only.
 *
 * WHY nothing else is here, deliberately:
 *   - No static-RSA key exchange (TLS_RSA_WITH_*). The client would encrypt the
 *     premaster secret to the server's certificate key, which (a) has no
 *     forward secrecy at all -- one leaked server key decrypts every recorded
 *     session ever -- and (b) is the Bleichenbacher oracle surface that has
 *     been re-broken every few years (ROBOT, 2017).
 *   - No CBC suites (TLS_..._WITH_AES_..._CBC_SHA). MAC-then-encrypt with an
 *     implicit-length pad is Lucky13, and the constant-time countermeasure is
 *     notoriously hard to get right in C.
 * Every server that would accept either of those also accepts ECDHE+GCM, so
 * supporting them buys no reachability and costs real attack surface. */
#define TLS_ECDHE_ECDSA_AES128_GCM_SHA256 0xC02B
#define TLS_ECDHE_RSA_AES128_GCM_SHA256   0xC02F
#define TLS_ECDHE_ECDSA_AES256_GCM_SHA384 0xC02C
#define TLS_ECDHE_RSA_AES256_GCM_SHA384   0xC030
#define TLS_ECDHE_ECDSA_CHACHA20_SHA256   0xCCA9
#define TLS_ECDHE_RSA_CHACHA20_SHA256     0xCCA8

/* AEAD algorithms, independent of which version selected them. */
#define AEAD_AES_128_GCM 1
#define AEAD_AES_256_GCM 2
#define AEAD_CHACHA20    3

/* Protocol versions, on the wire. Aliases for tls.h's public names so the two
 * headers cannot drift apart. */
#define TLS_V12 TLS_VER_12
#define TLS_V13 TLS_VER_13

/* named groups (RFC 8446 4.2.7; the same registry as RFC 4492's curves) */
#define GRP_P256    0x0017
#define GRP_P384    0x0018
#define GRP_X25519  0x001d

/* handshake message types */
#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_ENCRYPTED_EXT  8
#define HS_CERTIFICATE    11
#define HS_SERVER_KX      12                 /* TLS 1.2 ServerKeyExchange */
#define HS_CERT_REQUEST   13                 /* TLS 1.2 CertificateRequest */
#define HS_CERT_STATUS    22                 /* TLS 1.2 stapled OCSP, RFC 6066 8 */
#define HS_SERVER_DONE    14                 /* TLS 1.2 ServerHelloDone */
#define HS_CERT_VERIFY    15
#define HS_CLIENT_KX      16                 /* TLS 1.2 ClientKeyExchange */
#define HS_FINISHED       20
#define HS_KEY_UPDATE     24
#define HS_MESSAGE_HASH   254                /* synthetic, RFC 8446 4.4.1 */

/* record content types */
#define REC_CCS        20
#define REC_ALERT      21
#define REC_HANDSHAKE  22
#define REC_APPDATA    23

/* extensions */
#define EXT_SNI            0
#define EXT_STATUS_REQUEST 5                 /* OCSP stapling, RFC 6066 8 */
#define EXT_SUPPORTED_GRPS 10
#define EXT_EC_FORMATS     11
#define EXT_SIG_ALGS       13
#define EXT_ALPN           16
#define EXT_EMS            23                /* extended_master_secret, RFC 7627 */
#define EXT_PSK            41                /* pre_shared_key, MUST be last */
#define EXT_SUPPORTED_VERS 43
#define EXT_COOKIE         44
#define EXT_PSK_MODES      45                /* psk_key_exchange_modes */
#define EXT_KEY_SHARE      51
#define EXT_RENEG_INFO     0xff01            /* RFC 5746, empty */

/* psk_key_exchange_modes (RFC 8446 4.2.9). We only ever offer psk_dhe_ke:
 * resuming without a fresh ECDHE (psk_ke) gives up forward secrecy, which is
 * the property the rest of this stack exists to provide. */
#define PSK_KE             0
#define PSK_DHE_KE         1

#define HLEN 32                              /* SHA-256 transcript/HKDF (TLS 1.3) */

/* Largest TLSCiphertext.length RFC 8446 5.2 permits: 2^14 content + 256 for the
 * inner type, padding and the AEAD tag. TLS 1.2's AEAD ciphertext maxes out
 * lower (2^14 + 8 explicit nonce + 16 tag), so this bounds both. Anything
 * longer is a protocol error, so the receive buffer only ever needs one such
 * record plus its 5-byte header. */
#define REC_BODY_MAX  16640
#define RXBUF         (5 + REC_BODY_MAX + 16)

/* Outbound application records are capped well below the 2^14 maximum. That is
 * a deliberate memory trade: the send path has to hold a whole sealed record
 * until TCP accepts it (a half-written record is unparseable, so it cannot be
 * abandoned), and 16 sessions x 16 KiB of send buffer is a quarter megabyte of
 * kernel BSS bought for nothing -- a client's outbound traffic is requests,
 * which are small. Inbound is unaffected and still takes full-size records. */
#define SEND_REC_MAX  4096
#define TXBUF         (SEND_REC_MAX + 512)

/* The server's handshake flight. TLS 1.3: EncryptedExtensions + Certificate +
 * CertificateVerify + Finished; TLS 1.2: Certificate + ServerKeyExchange +
 * [CertificateRequest] + ServerHelloDone. Real chains run 4-6 KiB, including
 * the 4-cert RSA-4096 ones; 16 KiB has been enough for every chain we have met
 * and bounds what a hostile server can make us buffer. */
#define HSBUF         16384

/* Largest NewSessionTicket identity we will store. Real ones: openssl ~200
 * bytes, Google ~180, Cloudflare ~250, www.kimi.com 224. 512 accepts every one
 * met so far and bounds what a hostile server can make us hold per host. */
#define TICKET_BLOB_MAX 512

struct aead {
    int alg;                                 /* AEAD_* */
    uint8_t key[32]; int keylen;
    /* iv is the whole 12-byte nonce base for TLS 1.3 and for TLS 1.2
     * ChaCha20-Poly1305 (RFC 7905), which both XOR the sequence number into it.
     * For TLS 1.2 GCM (RFC 5288) it is a 4-byte salt and the remaining 8 bytes
     * travel in the clear at the front of every record -- see explicit_nonce. */
    uint8_t iv[12]; int ivlen;
    int explicit_nonce;                      /* 8 for TLS 1.2 GCM, else 0 */
    uint64_t seq;                            /* per-direction; reset by CCS in 1.2 */
};

/* Handshake-phase secrets (TLS 1.3). Kept in the session (not on the stack)
 * because the handshake is steppable -- it returns to the caller between
 * flights. Wiped the moment the application traffic keys are derived. */
struct hs_secrets {
    /* 48, not HLEN: TLS_AES_256_GCM_SHA384 runs the same schedule at SHA-384
     * width. Every one of these holds exactly s->hashlen bytes; the extra 16
     * are unused under a SHA-256 suite and are the price of one buffer size
     * instead of two code paths. */
    uint8_t hs[48];                          /* handshake secret */
    uint8_t c_hs[48], s_hs[48];              /* client/server handshake traffic */
};

enum {
    TS_FREE = 0,
    TS_SEND_CH,        /* build + queue ClientHello (first or post-HRR) */
    TS_RECV_SH,        /* read ServerHello, or a HelloRetryRequest */
    TS_RECV_FLIGHT,    /* TLS 1.3: read the encrypted EE..Finished flight */
    TS_FIN_FLUSH,      /* our Finished is queued; drain it */
    TS12_RECV_FLIGHT,  /* TLS 1.2: read Certificate..ServerHelloDone */
    TS12_RECV_CCS,     /* TLS 1.2: read the server's CCS + Finished */
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
    int ccs_sent;                            /* 1.3: the one compat CCS (RFC 8446 D.4) */
    uint8_t cookie[512]; int cookielen;      /* echoed back in CH2 if HRR sent one */

    int version;                             /* TLS_V13 / TLS_V12, 0 until ServerHello */
    int suite;
    int hashlen;                             /* transcript + PRF hash: 32 or 48 */

    /* Two transcripts run in lockstep from the first ClientHello byte, because
     * the hash is chosen by a cipher suite we have not been told yet: TLS 1.2's
     * *_SHA384 suites hash the handshake with SHA-384. Keeping both costs one
     * extra pass over a few KiB and removes the alternative, which is buffering
     * the whole handshake until the suite is known. */
    struct sha256 th;
    struct sha512 th384;

    struct hs_secrets sec;                   /* TLS 1.3 */
    struct aead cr, cw;                      /* read (server) / write (client) keys */

    /* --- TLS 1.3 session resumption (RFC 8446 2.2 / 4.6.1), see tls_psk.c ---
     * psk_offered says a pre_shared_key extension went out; psk_accepted says
     * the ServerHello came back naming it. The two are separate because a
     * server is free to ignore the offer and do a full handshake, and that
     * path must still work -- it is the common case the first time we meet a
     * host, and the fallback whenever a ticket has gone stale. */
    int     psk_offered, psk_accepted;
    int     psk_suite;                       /* the suite the ticket was issued under */
    uint8_t psk[HLEN];                       /* the resumption PSK we offered */
    /* The ticket is COPIED into the session at arm time and REMOVED from the
     * cache, because a TLS 1.3 ticket is single-use on most real servers. If
     * the session only held a key into a shared cache, two connections opened
     * at once would offer the same identity and the second would be refused --
     * which is exactly what a connection pool does on every page load. Costs
     * ~530 bytes per session (~8.5 KiB over 16) and that is the price of the
     * pool resuming more than once. */
    uint8_t  psk_id[TICKET_BLOB_MAX]; int psk_idlen;
    uint32_t psk_age_add;                    /* obfuscation addend, from the ticket */
    uint64_t psk_issued_ms;                  /* timer_ms() when it was received */
    uint8_t res_master[48];                  /* resumption_master_secret, once derived.
                                              * 48 for the same reason hs_secrets is;
                                              * only the SHA-256 case is ever STORED,
                                              * see tls_psk_store. */
    int     res_valid;                       /* res_master holds a real secret */

    /* --- TLS 1.2 --- */
    uint8_t srandom[32];                     /* ServerHello.random */
    uint8_t master[48];                      /* master secret */
    int ems;                                 /* extended_master_secret agreed */
    int cert_req;                            /* server asked for a client certificate */
    int rx_encrypted, tx_encrypted;          /* CCS has switched that direction on */
    uint8_t speer[97]; int speerlen;         /* server's ECDHE point */

    /* transport buffers */
    uint8_t rxrec[RXBUF]; int rxlen;         /* raw bytes / one partial record */
    int reclen; uint8_t rectype;             /* the record rec_pull() exposed */
    uint8_t tx[TXBUF]; int txlen, txoff;     /* sealed bytes waiting for TCP */
    uint8_t hsbuf[HSBUF]; int hslen;         /* server handshake flight */
    uint8_t app[REC_BODY_MAX]; int applen, appoff;   /* decrypted app data */

    /* The whole-handshake span. It begins in tls_start() and ends in one of
     * four places (1.3 done, 1.2 done, tls_fail, tls_close), which is why it
     * lives in the session rather than in a KPROF_BEGIN's function-local. */
    tls_prof_span prof;
};

/* --- provided by tls.c, used by tls12.c ---------------------------------- */

/* Terminal failure: wipes every secret, latches `rc`, returns it. */
int  tls_fail(struct tls_sess *s, int rc);

/* Non-blocking transport. tx_flush: 1 = drained, 0 = would block, -1 fatal.
 * tx_queue: frame body[len] as a record of `type` and queue it; -1 = no room.
 * rec_pull: 1 = s->rectype/s->reclen describe a record whose body starts at
 * s->rxrec+5, 0 = need more bytes, -1 = fatal. rec_drop consumes it. */
int  tls_tx_flush(struct tls_sess *s);
int  tls_tx_queue(struct tls_sess *s, uint8_t type, const uint8_t *body, int len);
int  tls_rec_pull(struct tls_sess *s);
void tls_rec_drop(struct tls_sess *s);

/* Transcript, fed to both hashes; tls_th_hash writes s->hashlen bytes. */
void tls_th_update(struct tls_sess *s, const void *p, int n);
void tls_th_hash(const struct tls_sess *s, uint8_t *out);

/* Dispatch to the AEAD the suite named (the framing differs between versions,
 * the primitive call does not). decrypt returns 0 on a good tag, -1 otherwise. */
void tls_aead_encrypt(const struct aead *a, const uint8_t nonce[12],
                      const uint8_t *aad, int aadlen,
                      const uint8_t *pt, int len, uint8_t *ct, uint8_t *tag);
int  tls_aead_decrypt(const struct aead *a, const uint8_t nonce[12],
                      const uint8_t *aad, int aadlen,
                      const uint8_t *ct, int len, const uint8_t *tag, uint8_t *pt);

/* Ephemeral key handling for s->group (x25519 / P-256 / P-384). */
int  tls_gen_share(struct tls_sess *s);
int  tls_compute_shared(struct tls_sess *s, const uint8_t *spub, int splen,
                        uint8_t *out, int *outlen);
int  tls_group_supported(int grp);
const char *tls_group_name(int grp);

/* Chain verification + the "say WHY it was refused" logging, shared by both
 * versions. 0 = fully trusted, TLS_E_CERT otherwise. */
int  tls_check_chain(struct tls_sess *s, const struct cert *chain, int ncert);

/* The stapled-OCSP decision, shared by both versions. `staple` may be NULL
 * (most servers do not staple), which is not an error -- see ocsp.h for the
 * policy and tls.c for the one case that is neither good nor bad. 0 = proceed,
 * TLS_E_CERT = the staple said, or implied, no. */
int  tls_check_staple(struct tls_sess *s, const struct cert *chain, int ncert,
                      const uint8_t *staple, int staplelen);

void tls_log_alert(const uint8_t *body, int blen);

/* Record the server's ALPN choice from an ALPN extension body. TLS 1.3 carries
 * it in EncryptedExtensions, TLS 1.2 in the ServerHello; same encoding. */
void tls_take_alpn(struct tls_sess *s, const uint8_t *ext, int el);

/* --- provided by tls_psk.c (TLS 1.3 session resumption) ------------------ */

/* Load a cached PSK for s->host into s->psk. 1 = armed, 0 = full handshake. */
int  tls_psk_arm(struct tls_sess *s);

/* Extension builders. modes_ext is unconditional whenever a PSK is offered;
 * psk_ext must be written LAST in the ClientHello and reports where the binder
 * transcript ends (*trunc) and where the binder value sits (*binder), both
 * relative to `p`. Both return bytes written, or -1 if they would not fit. */
int  tls_psk_modes_ext(uint8_t *p, int max);
int  tls_psk_ext(struct tls_sess *s, uint8_t *p, int max, int *trunc, int *binder);

/* Patch the binder into a built ClientHello. `th` is the transcript BEFORE
 * this ClientHello (empty on a first flight, post-HRR state after a retry);
 * `trunc_len` and `binder_off` are offsets into `ch`. */
void tls_psk_binder(struct tls_sess *s, const struct sha256 *th,
                    uint8_t *ch, int trunc_len, int binder_off);

/* Cache a NewSessionTicket. Returns 0 stored, -1 refused (bad shape, dead
 * lifetime, unusable suite). */
int  tls_psk_store(const char *host, int suite, const uint8_t *res_master,
                   const uint8_t *nonce, int noncelen,
                   const uint8_t *blob, int bloblen,
                   uint32_t lifetime, uint32_t age_add, int64_t now);

/* Drop a host's ticket -- called when the server refuses our binder. */
void tls_psk_forget(const char *host);

/* Diagnostics / tests. */
int  tls_psk_count(void);
void tls_psk_clear_all(void);

/* --- provided by tls12.c ------------------------------------------------- */

/* Enter the TLS 1.2 handshake. The ServerHello has already been validated and
 * hashed into the transcript by tls.c; sh gives its body so the extensions can
 * be re-read. Returns a tls_step() result. */
int  tls12_begin(struct tls_sess *s);

/* Advance a TLS 1.2 handshake in TS12_* state. Returns a tls_step() result. */
int  tls12_step(struct tls_sess *s);

/* TLS 1.2 record layer, post-handshake. read: 1 = a record was produced
 * (*ctype and *len describe s->app), 0 = need more bytes, -1 = fatal.
 * write: seal pt[len] as one record of content type `type` and queue it. */
int  tls12_read_record(struct tls_sess *s, uint8_t *ctype, int *len);
int  tls12_write_record(struct tls_sess *s, uint8_t type, const uint8_t *pt, int len);

#endif /* LOGIT_TLS_INT_H */
