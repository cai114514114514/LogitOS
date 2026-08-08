/* TLS 1.3 session resumption (RFC 8446 §2.2, §4.2.11, §4.6.1): the ticket
 * cache, the PSK binder, and the pre_shared_key / psk_key_exchange_modes
 * extensions.
 *
 * WHY this exists, in one number: a page with 40 sub-resources over a
 * connection pool that keeps missing pays 40 full handshakes, and a full
 * handshake on this target is dominated by one ECDHE key generation plus a
 * chain verification (an RSA-4096 modexp in the worst case). A resumed
 * handshake does neither: the server is authenticated by the PSK binder
 * instead of by a signature over a certificate chain, so the entire
 * Certificate / CertificateVerify flight disappears along with the X.509 work.
 * The ECDHE is still there -- we only ever offer psk_dhe_ke, never bare
 * psk_ke -- because that is what keeps forward secrecy, and giving that up to
 * save a scalar multiplication would be trading the property the whole stack
 * exists to provide for a few milliseconds.
 *
 * --- what a resumed handshake actually is ---
 * The server hands out a NewSessionTicket after the handshake. From it and the
 * resumption_master_secret we derive a PSK. Next time we lead the ClientHello
 * with that PSK's identity (the opaque ticket blob) and prove we hold the PSK
 * with a *binder*: an HMAC, keyed by a value derived from the PSK, over the
 * ClientHello truncated immediately before the binders themselves. The server
 * recomputes it; if it matches, it answers with pre_shared_key naming the
 * identity it chose, and the key schedule starts from the PSK instead of from
 * zeros. The certificate flight never happens.
 *
 * --- the two things that are easy to get wrong here ---
 * 1. The binder is computed over a *truncated* ClientHello: everything up to
 *    and including the identities list, and NOTHING of the binders list -- not
 *    even its 2-byte length prefix. Off by those two bytes and the server
 *    rejects the binder, which on most servers is a silent fallback to a full
 *    handshake, so the bug shows up as "resumption never works" rather than as
 *    an error. tls_psk_binder() takes the truncation offset the builder
 *    recorded rather than recomputing it, so the two cannot drift.
 * 2. pre_shared_key MUST be the last extension in the ClientHello (RFC 8446
 *    §4.2.11). It is the only extension with that requirement, and it exists
 *    precisely so the truncation point above is well defined.
 *
 * --- constant time ---
 * The binder comparison is not ours to make (the server makes it). What IS
 * ours: tls_psk_find() matches on a HOST NAME, which is not secret, so its
 * early-out is fine. Nothing in this file branches on PSK bytes. The PSK and
 * resumption_master_secret are wiped by crypto_wipe on eviction and on
 * forget.
 */

#include <stdint.h>
#include <stddef.h>
#include "tls_int.h"
#include "crypto.h"
#include "kprintf.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);

/* A TLS 1.3 ticket is SINGLE-USE on most real servers, and servers issue them
 * in batches precisely so a client can open several connections. www.kimi.com
 * hands out eight per handshake. The first version of this cache kept ONE
 * ticket per host -- it threw seven of those eight away and then offered the
 * survivor to every connection in the pool, so the second and third connections
 * of a page load were refused and fell back to a full handshake. That is what
 * the field data showed, and it is why the cache is now a flat pool that holds
 * several tickets per host and REMOVES one when it is offered.
 *
 * 16 entries at ~810 bytes is ~13 KiB of kernel BSS, sized to hold one server's
 * batch for two origins at once. */
#define TICKET_MAX      16

/* The millisecond clock, in ONE place so the store side and the age side can
 * never disagree about the unit -- which is a way to be wrong that produces a
 * plausible-looking number rather than an obvious one.
 *
 * timer_ms() and not timer_ticks(): a tick is 1000/TIMER_HZ = 10 ms, and pit.h
 * says outright that callers should ask rather than open-code that. Reading the
 * tick as ms is what shipped first, and it understated every
 * obfuscated_ticket_age by a factor of ten. */
#ifdef LOGIT_PSK_BREAK_AGE_UNIT
/* NEGATIVE CONTROL (test-tls-psk-control): the original mistake, restored. */
#  define PSK_NOW_MS() timer_ticks()
#else
#  define PSK_NOW_MS() timer_ms()
#endif

struct tls_ticket {
    int      used;
    char     host[256];
    int      suite;                  /* the suite it was issued under */
    uint8_t  psk[HLEN];              /* PSK = Expand-Label(res_master, "resumption", nonce) */
    uint8_t  blob[TICKET_BLOB_MAX];  /* the opaque identity we send back */
    int      bloblen;
    uint32_t lifetime;               /* seconds the server promises to honour it */
    uint32_t age_add;                /* obfuscation addend for the age, in ms */
    int64_t  issued;                 /* unix seconds at receipt, for expiry */
    /* timer_ms(), NOT timer_ticks(). The tick is 10 ms at TIMER_HZ=100, so the
     * first version of this reported an obfuscated_ticket_age ten times too
     * small -- pit.h warns about exactly this ("callers should ask rather than
     * open-code 1000/TIMER_HZ"). RFC 8446 4.2.11.1 specifies milliseconds, and
     * a server that range-checks the age against its own elapsed time sees a
     * number that cannot be right. */
    uint64_t issued_ms;              /* timer_ms() at receipt, for the age */
    uint64_t seq;                    /* arrival order, so a batch is used FIFO */
};

static struct tls_ticket tickets[TICKET_MAX];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void ticket_clear(struct tls_ticket *t) { crypto_wipe(t, sizeof *t); }

/* --------------------------------------------------------------- the cache */

/* RFC 8446 §4.6.1 caps ticket_lifetime at 7 days; a server claiming more is
 * either broken or trying to make us hold key material indefinitely, so the
 * value is clamped rather than trusted. */
#define TICKET_LIFETIME_MAX 604800

static uint64_t ticket_seq;

/* A free slot, or the least recently issued one. Tickets are NOT deduplicated
 * by host any more -- see the note at TICKET_MAX: a batch of eight is eight
 * separate single-use credentials, and collapsing them to one is throwing away
 * seven resumptions. */
static struct tls_ticket *ticket_slot(void)
{
    struct tls_ticket *oldest = &tickets[0];
    for (int i = 0; i < TICKET_MAX; i++) {
        if (!tickets[i].used) return &tickets[i];
        if (tickets[i].seq < oldest->seq) oldest = &tickets[i];
    }
    ticket_clear(oldest);                    /* evict the oldest, wiping its PSK */
    return oldest;
}

/* Store a NewSessionTicket. `res_master` is the resumption_master_secret of the
 * session that issued it; the PSK is derived here rather than stored raw so the
 * resumption secret itself never outlives its session. Returns 0 on success. */
int tls_psk_store(const char *host, int suite, const uint8_t *res_master,
                  const uint8_t *nonce, int noncelen,
                  const uint8_t *blob, int bloblen,
                  uint32_t lifetime, uint32_t age_add, int64_t now)
{
    if (!host || !host[0] || bloblen <= 0 || bloblen > TICKET_BLOB_MAX) return -1;
    /* hkdf_expand_label bounds its context at 64 bytes. A ticket_nonce is
     * 0..255 by the grammar but every implementation in the wild uses 8; a
     * longer one is refused rather than silently truncated, because a
     * truncated nonce derives the WRONG PSK and that failure is invisible
     * until the binder is rejected. */
    if (noncelen < 0 || noncelen > 64) return -1;
    if (lifetime == 0) return -1;            /* an already-dead ticket */
    if (lifetime > TICKET_LIFETIME_MAX) lifetime = TICKET_LIFETIME_MAX;
    /* Only the two TLS 1.3 suites resume, and both are SHA-256; a ticket from
     * anything else could not be keyed correctly. */
    if (suite != TLS_AES_128_GCM_SHA256 && suite != TLS_CHACHA20_POLY1305_SHA256)
        return -1;

    struct tls_ticket *t = ticket_slot();
    ticket_clear(t);

    int i = 0;
    while (host[i] && i < (int)sizeof t->host - 1) { t->host[i] = host[i]; i++; }
    t->host[i] = 0;

    if (hkdf_expand_label(HLEN, res_master, "resumption", nonce, noncelen,
                          t->psk, HLEN) != 0) {
        ticket_clear(t);
        return -1;
    }
    memcpy(t->blob, blob, (size_t)bloblen);
    t->bloblen  = bloblen;
    t->suite    = suite;
    t->lifetime = lifetime;
    t->age_add  = age_add;
    t->issued   = now;
    t->issued_ms = PSK_NOW_MS();             /* ms, not ticks -- see PSK_NOW_MS */
    t->seq      = ++ticket_seq;
    t->used     = 1;
    kprintf("[tls] ticket stored for %s (%d bytes, lifetime %us, %d cached)\n",
            t->host, bloblen, lifetime, tls_psk_count());
    return 0;
}

/* The oldest live ticket for `host`, or NULL. Expired ones are dropped as they
 * are passed rather than offered and refused.
 *
 * FIFO within a host: a batch is used in the order the server issued it, which
 * is the order its own replay window expects. */
static struct tls_ticket *ticket_find(const char *host, int64_t now)
{
    struct tls_ticket *best = 0;
    for (int i = 0; i < TICKET_MAX; i++) {
        struct tls_ticket *t = &tickets[i];
        if (!t->used || !streq(t->host, host)) continue;
        /* now < issued guards a clock that went backwards (the RTC is the
         * source, and it can be corrected under us); treat it as expired
         * rather than computing a huge negative age. */
        if (now < t->issued || now - t->issued >= (int64_t)t->lifetime) {
            kprintf("[tls] ticket for %s expired -- discarded\n", t->host);
            ticket_clear(t);
            continue;
        }
        if (!best || t->seq < best->seq) best = t;
    }
    return best;
}

/* Drop EVERY ticket for `host`. Called when a server refuses the identity we
 * offered.
 *
 * Dropping the whole batch is only correct because arm() consumes: the ticket
 * we offered is already gone, so a refusal can no longer mean "you reused one"
 * -- it means the server will not resume us at all (rotated STEK, policy
 * change, ticket already redeemed elsewhere). In that state the other tickets
 * in the batch are dead too, and re-offering them costs one rejected binder
 * each. Before arm() consumed, this same line was throwing away seven live
 * tickets every time one got double-offered. */
void tls_psk_forget(const char *host)
{
    for (int i = 0; i < TICKET_MAX; i++)
        if (tickets[i].used && streq(tickets[i].host, host)) ticket_clear(&tickets[i]);
}

/* Test/diagnostic: how many live tickets are cached. */
int tls_psk_count(void)
{
    int n = 0;
    for (int i = 0; i < TICKET_MAX; i++) if (tickets[i].used) n++;
    return n;
}

void tls_psk_clear_all(void)
{
    for (int i = 0; i < TICKET_MAX; i++) ticket_clear(&tickets[i]);
}

/* ------------------------------------------------- building the ClientHello */

static int put_u16(uint8_t *p, int v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; return 2; }

/* Take a ticket for this session's host and CONSUME it -- the whole identity is
 * copied into the session and the cache entry is wiped. Returns 1 if the
 * session is armed, 0 for a full handshake.
 *
 * Consuming rather than borrowing is the fix the field data asked for. A TLS
 * 1.3 ticket is single-use on most production servers; leaving it in the cache
 * meant that two connections opened at once -- i.e. what a connection pool does
 * on every page load -- offered the SAME identity, and the server refused the
 * second. Against www.kimi.com that was two of three connections falling back
 * to a full handshake while seven perfectly good tickets sat discarded.
 *
 * The cost of consuming is that a handshake which fails for some unrelated
 * reason burns the ticket. That is the right trade: a ticket is cheap and the
 * server just issued eight of them, whereas re-offering a used one is a
 * guaranteed rejected binder. */
int tls_psk_arm(struct tls_sess *s)
{
    struct tls_ticket *t = ticket_find(s->host, s->now);
    if (!t) { s->psk_offered = 0; return 0; }
    memcpy(s->psk, t->psk, HLEN);
    memcpy(s->psk_id, t->blob, (size_t)t->bloblen);
    s->psk_idlen     = t->bloblen;
    s->psk_suite     = t->suite;
    s->psk_age_add   = t->age_add;
    s->psk_issued_ms = t->issued_ms;
    s->psk_offered   = 1;
#ifndef LOGIT_PSK_BREAK_SINGLE_USE
    ticket_clear(t);                         /* single-use: nobody else gets it */
#else
    /* NEGATIVE CONTROL (test-tls-psk-control): leave the ticket in the cache,
     * which is what the first version did. Everything still "works" against a
     * lenient server; against a single-use one, every connection after the
     * first is refused. */
#endif
    return 1;
}

/* psk_key_exchange_modes (RFC 8446 §4.2.9). Mandatory whenever pre_shared_key
 * is present. We advertise psk_dhe_ke ONLY -- see the file header: bare psk_ke
 * would let the server resume without a fresh ECDHE, which is exactly the
 * forward secrecy we are not willing to trade away. */
int tls_psk_modes_ext(uint8_t *p, int max)
{
    if (max < 6) return -1;
    int n = 0;
    n += put_u16(p + n, EXT_PSK_MODES);
    n += put_u16(p + n, 2);
    p[n++] = 1;                              /* modes list length */
    p[n++] = PSK_DHE_KE;
    return n;
}

/* The pre_shared_key extension, with the binder left as zeros for
 * tls_psk_binder() to fill in.
 *
 * `*trunc_out` receives the offset (relative to p) of the binders-list length
 * field -- i.e. the length of the part that goes into the binder transcript.
 * `*binder_out` receives the offset of the binder VALUE itself. Returning both
 * rather than letting the caller recompute them is deliberate: the two-byte
 * gap between them is the single easiest thing to get wrong in resumption. */
int tls_psk_ext(struct tls_sess *s, uint8_t *p, int max,
                int *trunc_out, int *binder_out)
{
    /* Built from the session's own copy, taken by tls_psk_arm(). There is no
     * cache lookup here on purpose: arm() consumed the entry, and a second
     * lookup is how the same identity ends up on two connections. */
    if (!s->psk_offered || s->psk_idlen <= 0) return -1;

    /* ext hdr(4) + identities_len(2) + id_len(2) + blob + age(4)
     *            + binders_len(2) + binder_len(1) + binder(32) */
    int need = 4 + 2 + 2 + s->psk_idlen + 4 + 2 + 1 + HLEN;
    if (max < need) return -1;

    int n = 0;
    n += put_u16(p + n, EXT_PSK);
    n += put_u16(p + n, need - 4);

    /* --- identities --- */
    n += put_u16(p + n, 2 + s->psk_idlen + 4);        /* identities list length */
    n += put_u16(p + n, s->psk_idlen);
    memcpy(p + n, s->psk_id, (size_t)s->psk_idlen); n += s->psk_idlen;

    /* obfuscated_ticket_age (RFC 8446 §4.2.11.1): the age of the ticket in
     * MILLISECONDS plus the server's age_add, modulo 2^32. The server uses it
     * to bound replay; the addend stops a passive observer correlating two
     * connections by their ticket age. Wrapping is not an error here -- the
     * arithmetic is specified modulo 2^32, which is what uint32_t does.
     *
     * timer_ms(), NOT timer_ticks(): a tick is 10 ms at TIMER_HZ=100, so using
     * the raw tick made every age we ever sent ten times too small. It is the
     * exact mistake pit.h warns callers about, and it is invisible from our
     * side -- the only symptom is a server declining the ticket. */
    uint64_t now_ms = PSK_NOW_MS();
    uint32_t age_ms = (uint32_t)(now_ms >= s->psk_issued_ms ? now_ms - s->psk_issued_ms : 0);
    uint32_t obf = age_ms + s->psk_age_add;
    p[n++] = (uint8_t)(obf >> 24); p[n++] = (uint8_t)(obf >> 16);
    p[n++] = (uint8_t)(obf >> 8);  p[n++] = (uint8_t)obf;

    *trunc_out = n;                          /* binder transcript ends HERE */

    /* --- binders (zero-filled; patched by tls_psk_binder) --- */
    n += put_u16(p + n, 1 + HLEN);           /* binders list length */
    p[n++] = HLEN;                           /* this binder's length */
    *binder_out = n;
    memset(p + n, 0, HLEN); n += HLEN;

    return n;
}

/* Compute and patch the binder.
 *
 *   early_secret = HKDF-Extract(salt=0, IKM=PSK)
 *   binder_key   = Derive-Secret(early_secret, "res binder", "")
 *   finished_key = HKDF-Expand-Label(binder_key, "finished", "", Hash.length)
 *   binder       = HMAC(finished_key, Transcript-Hash(Truncate(ClientHello)))
 *
 * `th` is the transcript as it stands BEFORE this ClientHello -- empty for a
 * first flight, and the synthetic message_hash||HelloRetryRequest state after a
 * retry. Passing it in rather than assuming "empty" is what makes resumption
 * survive an HRR: the binder must be recomputed over the new transcript, and a
 * client that reuses the first binder is rejected.
 */
void tls_psk_binder(struct tls_sess *s, const struct sha256 *th,
                    uint8_t *ch, int trunc_len, int binder_off)
{
    uint8_t early[HLEN], binder_key[HLEN], fk[HLEN], thash[HLEN], emptyhash[HLEN];
    uint8_t zeros_salt[HLEN];
    memset(zeros_salt, 0, HLEN);

    hkdf_extract(HLEN, 0, 0, s->psk, HLEN, early);
    sha256("", 0, emptyhash);
    hkdf_expand_label(HLEN, early, "res binder", emptyhash, HLEN, binder_key, HLEN);
    hkdf_expand_label(HLEN, binder_key, "finished", 0, 0, fk, HLEN);

    /* Transcript-Hash(Truncate(ClientHello)): the running transcript extended
     * by the truncated ClientHello, not a hash of the truncation alone. */
    struct sha256 c = *th;
    sha256_update(&c, ch, (size_t)trunc_len);
    sha256_final(&c, thash);

    hmac(HLEN, fk, HLEN, thash, HLEN, ch + binder_off);

    crypto_wipe(early, sizeof early);
    crypto_wipe(binder_key, sizeof binder_key);
    crypto_wipe(fk, sizeof fk);
    crypto_wipe(&c, sizeof c);
}
