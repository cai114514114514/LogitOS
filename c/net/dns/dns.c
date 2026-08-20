#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "dns.h"
#include "udp.h"
#include "tcp.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "rng.h"
#include "ip6.h"

/* A DNS client for LogitOS.
 *
 * WHAT THIS IS, PLAINLY: A STUB RESOLVER. It asks exactly ONE upstream server
 * (net_cfg.dns -- DHCP option 6, or the static SLIRP fallback 10.0.2.3) and
 * trusts whatever it says. There is no local root-hints walk, no iterative
 * referral-following, no DNSSEC validation -- all of "how do I turn a name
 * into an address" past the first hop is outsourced to that one configured
 * server, which is what "stub resolver" means (RFC 1034 s5.3.1). That is the
 * right amount of resolver for a client that always has exactly one,
 * DHCP-handed, trusted-by-construction upstream; it is not a placeholder for
 * a fuller resolver written later. If this file ever grows real recursion or
 * DNSSEC, THIS PARAGRAPH IS WHAT MUST CHANGE, not just the roadmap notes.
 *
 * WHAT IT DOES beyond the stub minimum, and why each exists (2026-08-20):
 *   - it checks the OWNER NAME of every record before accepting it, and
 *     follows CNAME chains explicitly (bounded, DNS_MAX_CNAME hops), rather
 *     than taking any A/AAAA record anywhere in the answer section as the
 *     answer to OUR question. Skipping this was a real bug, not a
 *     simplification: a response carrying an unrelated name's address
 *     alongside ours, or one carrying only a CNAME's final target, used to
 *     be accepted as ours regardless of whose name it actually named.
 *   - EDNS0 (RFC 6891): every query advertises a receive size of
 *     DNS_MSG_MAX (4096), so the classic 512-byte UDP cap essentially never
 *     truncates a real answer. When a response still comes back with TC set,
 *     it is re-asked over TCP (length-prefixed, RFC 1035 s4.2.2) instead of
 *     being accepted as-is -- a truncated UDP response is not a partial
 *     answer, it is an UNRELIABLE one, and taking it at face value is how a
 *     resolver silently drops the very records truncation warns are missing.
 *   - the TTL is read off the record (clamped to [DNS_TTL_FLOOR,
 *     DNS_TTL_CEIL]) instead of a fixed 120 s. See those two constants for
 *     what each bound is for.
 *
 * IPv6 changes the SHAPE of a lookup, not just the record type. A name has an
 * A set and a AAAA set, they arrive in separate transactions that finish at
 * different times, and the caller wants ONE list in preference order -- which
 * is RFC 6724 destination ordering, not "v6 first". So the async pool below
 * runs both queries at once, merges the answers into a family-tagged list
 * (IPv4 carried as ::ffff:a.b.c.d, which is how RFC 6724 compares the two),
 * and hands the ordered list to sock.c.
 *
 * The transport stays IPv4 for UDP (c/net/transport/ has no UDP over IPv6);
 * asking an IPv4 resolver for AAAA records is normal and complete -- what a
 * stack must never do is assume that no IPv6 transport means no IPv6
 * addresses. The IPv4 TCP fallback runs over the same tcp.c client http.c
 * already uses.
 *
 * AAAA is only asked for when ip6_up() reports a routable address AND a
 * default router. On a v4-only network not one extra byte goes on the wire
 * and this file behaves exactly as it did before IPv6 existed.
 *
 * THE LEGACY dns_start()/dns_result() API and the async pool
 * (dns_query_start()/dns_poll()) below share ONE parsing/state-advancement
 * implementation (dq_advance / collect_answers), on purpose: the two used to
 * be independent copies, and a name-matching or TTL fix landing in only one
 * of them is exactly the "cookie jar, two doors" shape this tree has been
 * bitten by before -- a gate aimed at a rule has to be aimed at every caller
 * of the rule. The one deliberate behavioural difference that survives the
 * merge is that dns_start()/dns_resolve() never reads the name cache (see
 * dns_start()'s comment) -- everything else is identical code. */

/* ---- constants ------------------------------------------------------------ */

#define DNS_PORT    53
#define DNS_T_A     1
#define DNS_T_AAAA  28
#define DNS_T_CNAME 5

/* Longest answer set kept per name. Eight covers every real round-robin set
 * worth trying before giving up on a host. */
#define DNS_MAXA    8

/* How many CNAMEs one response may chain through before we stop following.
 * RFC 1035 sets no limit; a real chain (CDN indirection) is 1-3 hops. Eight
 * bounds the cost of a hostile chain to a fixed, small number of extra
 * comparisons rather than a search over the whole answer section per hop. */
#define DNS_MAX_CNAME 8

/* How many compression-pointer redirections read_name() will follow while
 * decoding one name. A real name needs at most a couple (an owner pointing
 * back at the question, say); this is headroom, not a budget anyone should
 * ever spend all of. It is what actually defeats a pointer LOOP -- see
 * read_name()'s comment for why the "point strictly backward" rule alone
 * does not, by itself, prove termination. */
#define DNS_MAX_PTR_JUMPS 20

/* Both the EDNS0 UDP payload size we advertise/accept and the cap on a
 * DNS-over-TCP response we will parse. One constant on purpose: advertising
 * a bigger receive size than the buffer that actually backs it is its own
 * bug class. 4096 is what most resolvers themselves advertise -- big enough
 * that a real answer essentially never truncates, not 65535 (which would
 * invite a fragmented UDP reply, exactly the kind of packet an off-path
 * spoofer benefits from and several middleboxes drop outright). */
#define DNS_MSG_MAX  4096

/* Upper bound on an OUTGOING query we build (name <= 253 + header + EDNS0
 * OPT is nowhere near this; it is shared by every query-building buffer in
 * this file so there is one number to check against overflow). */
#define DNS_QMSG_MAX 512

/* TTL floor and ceiling, in SECONDS, applied to whatever the record itself
 * says (see collect_answers()). The old code did not read the TTL at all --
 * a fixed 120 s, argued as "short enough that a re-pointed name recovers
 * within a page reload, long enough that a page's thirty-odd sub-resources
 * cost one lookup per host". Reading the real value and clamping it keeps
 * both properties for the common case while still respecting an operator's
 * legitimate TTL choice within bounds:
 *   FLOOR 5 s: a 0 s/1 s TTL is legitimate (a load balancer doing
 *   per-request DNS rotation), but re-querying on literally every
 *   sub-resource of one page is real cost for no benefit to a browser that
 *   only ever holds one TCP connection per host at a time. A short floor
 *   also raises the bar on DNS REBINDING: an attacker who wants this name to
 *   re-resolve to a different address between a same-origin check and the
 *   connection that follows it has to win a race against this floor, not
 *   against a TTL of their own choosing.
 *   CEILING 3600 s (1 h): bounds how long a stale record can survive a
 *   re-pointed name or an operator's mistaken very-large TTL. The old fixed
 *   value was 120 s for the same reason (recover within a session); 3600 s
 *   is generous by comparison -- most CDNs advertise 60-3600 s anyway -- and
 *   still recovers well inside a single desktop session. */
#define DNS_TTL_FLOOR 5
#define DNS_TTL_CEIL  3600

/* ---- names: build, skip, decode, compare ---------------------------------- */

/* Encode "a.b.c" as length-prefixed labels into out; returns bytes written. */
static int encode_qname(uint8_t *out, const char *name)
{
    int o = 0;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int lbl = (int)(dot - p);
        if (lbl > 63) return -1;
        out[o++] = (uint8_t)lbl;
        for (int i = 0; i < lbl; i++) out[o++] = (uint8_t)p[i];
        p = (*dot == '.') ? dot + 1 : dot;
    }
    out[o++] = 0;       /* root label */
    return o;
}

/* Skip a (possibly compressed) DNS name starting at off; return new offset.
 * Does not follow a pointer's target -- a compression pointer is always the
 * LAST element of a name (RFC 1035 s4.1.4), so the byte right after it is
 * where the caller's next field starts, whether or not anyone ever reads
 * what the pointer points at. read_name() below is the one that follows it,
 * for names we actually need the text of. */
static int skip_name(const uint8_t *msg, int off, int len)
{
    int depth = 0;
    while (off < len && depth < 64) {
        uint8_t l = msg[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2;     /* compression pointer */
        off += 1 + l;
        depth++;
    }
    return off;
}

/* Lower-cased copy of `name`, presentation form, with exactly one trailing
 * '.' -- the same canonical form read_name() produces for a decoded wire
 * name, so ownership can be checked with a plain strcmp() rather than a
 * DNS-aware equality routine that would have to special-case the dot. */
static void canon_name(char *out, int outmax, const char *name)
{
    int i = 0;
    for (; name[i] && i < outmax - 2; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[i] = c;
    }
    if (i == 0 || out[i - 1] != '.') out[i++] = '.';
    out[i] = 0;
}

/* Decode a (possibly compressed) name starting at `off` into `out` --
 * lower-cased, dot-terminated after every label including the last (so
 * "www"+"example"+"com" becomes "www.example.com."), NUL-terminated and
 * truncated rather than overflowing if it does not fit `outmax`. Returns the
 * offset just past the name AS IT APPEARS AT THE CALL SITE (a pointer counts
 * as 2 bytes there, same contract as skip_name), or -1 if the name is
 * malformed: a label or pointer running past `len`, a pointer that does not
 * point strictly backward (RFC 1035 compression may only reference a PRIOR
 * occurrence -- a forward or self pointer is invalid on its own), or a
 * pointer chain longer than DNS_MAX_PTR_JUMPS hops.
 *
 * The backward-only rule is necessary but NOT sufficient to prove
 * termination by itself: label traversal between two pointer jumps can
 * advance `pos` past where an earlier jump landed, so a chain of jumps that
 * are each individually backward relative to their OWN position can still
 * revisit the same neighbourhood indefinitely (jump to 5, read labels
 * forward to 20, jump backward to 15, read forward to 25, jump backward to
 * 5, ...). DNS_MAX_PTR_JUMPS is the actual termination guarantee; the
 * backward-only check is spec-correctness on top of it, not instead of it. */
static int read_name(const uint8_t *msg, int off, int len, char *out, int outmax)
{
    int pos = off;
    int end = -1;
    int jumps = 0;
    int olen = 0;
    if (outmax > 0) out[0] = 0;
    for (;;) {
        if (pos < 0 || pos >= len) return -1;
        uint8_t l = msg[pos];
        if (l == 0) { if (end < 0) end = pos + 1; break; }
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return -1;
            int ptr = ((l & 0x3F) << 8) | msg[pos + 1];
            if (end < 0) end = pos + 2;
            if (ptr >= pos) return -1;          /* must point strictly backward */
            if (++jumps > DNS_MAX_PTR_JUMPS) return -1;
            pos = ptr;
            continue;
        }
        if (pos + 1 + l > len) return -1;
        for (int i = 0; i < l; i++) {
            uint8_t c = msg[pos + 1 + i];
            if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
            if (olen + 1 < outmax) out[olen++] = (char)c;
        }
        if (olen + 1 < outmax) out[olen++] = '.';
        pos += 1 + l;
    }
    if (olen == 0 && outmax > 1) { out[0] = '.'; olen = 1; }
    out[olen < outmax ? olen : outmax - 1] = 0;
    return end;
}

/* Dotted-decimal IP literal "a.b.c.d" -> host-order IP, or 0 if not a literal. */
static uint32_t parse_ip_literal(const char *s)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, ndig = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++ndig > 3 || v > 255) return 0; }
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    return *s == 0 ? ip : 0;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void name_copy(char *d, const char *s)
{
    int i = 0;
    for (; i < DNS_NAME_MAX - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* Build an A/AAAA query for `name` into q; returns its length, or -1. The
 * transaction id is generated here and handed back through *out_txid, so each
 * concurrent query in the async pool (and each TCP fallback attempt) keeps
 * its own spoofing guard rather than sharing one module-global.
 *
 * Every query carries an EDNS0 OPT pseudo-RR (RFC 6891) advertising a
 * DNS_MSG_MAX receive size -- see that constant for why 4096 and not 65535. */
static int build_query_type(uint8_t *q, const char *name, int qtype,
                            uint16_t *out_txid)
{
    uint16_t id;
    kernel_random_bytes((uint8_t *)&id, sizeof id);
    id ^= (uint16_t)timer_ticks();
    *out_txid = id;
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)(id & 0xFF);
    q[2] = 0x01; q[3] = 0x00;                       /* RD=1 */
    q[4] = 0; q[5] = 1;                             /* QDCOUNT=1 */
    q[6] = 0; q[7] = 0;                             /* ANCOUNT=0 */
    q[8] = 0; q[9] = 0;                             /* NSCOUNT=0 */
    q[10] = 0; q[11] = 1;                           /* ARCOUNT=1 (the EDNS0 OPT) */
    int o = 12;
    int n = encode_qname(q + o, name);
    if (n < 0) return -1;
    o += n;
    q[o++] = (uint8_t)(qtype >> 8); q[o++] = (uint8_t)qtype;
    q[o++] = 0; q[o++] = 1;                         /* QCLASS IN */
    /* EDNS0 OPT: NAME=root, TYPE=41, CLASS=advertised UDP size, TTL carries
     * the extended-rcode/version/flags (0: no DNSSEC OK, no extended rcode),
     * RDLENGTH=0 (no options). */
    q[o++] = 0;
    q[o++] = 0; q[o++] = 41;
    q[o++] = (uint8_t)(DNS_MSG_MAX >> 8); q[o++] = (uint8_t)DNS_MSG_MAX;
    q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0;
    q[o++] = 0; q[o++] = 0;
    return o;
}

/* ---- the name cache -------------------------------------------------------- */

#define DNS_NCACHE  16

struct dns_cent {
    char     name[DNS_NAME_MAX];
    uint32_t ip;                    /* first A record (the pre-IPv6 field) */
    ip6_addr addrs[DNS_MAXA];       /* the full mixed-family set */
    uint8_t  naddr;
    uint64_t exp;
};
static struct dns_cent dcache[DNS_NCACHE];
static int dcache_w;                /* round-robin victim: no LRU, 16 entries */

static struct dns_cent *dns_cache_find(const char *name)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < DNS_NCACHE; i++)
        if (dcache[i].naddr && dcache[i].exp > now && name_eq(dcache[i].name, name))
            return &dcache[i];
    return NULL;
}

/* Store the full answer set, expiring after `ttl_secs` clamped to
 * [DNS_TTL_FLOOR, DNS_TTL_CEIL]. */
static void dns_cache_put_set(const char *name, const ip6_addr *a, int n, uint32_t ttl_secs)
{
    if (n <= 0) return;
    if (n > DNS_MAXA) n = DNS_MAXA;
    if (ttl_secs < DNS_TTL_FLOOR) ttl_secs = DNS_TTL_FLOOR;
    if (ttl_secs > DNS_TTL_CEIL)  ttl_secs = DNS_TTL_CEIL;
    uint64_t now = timer_ticks();
    struct dns_cent *e = NULL;
    for (int i = 0; i < DNS_NCACHE; i++)          /* refresh an existing entry */
        if (dcache[i].naddr && name_eq(dcache[i].name, name)) { e = &dcache[i]; break; }
    if (!e) {
        e = &dcache[dcache_w++ % DNS_NCACHE];
        name_copy(e->name, name);
    }
    e->naddr = (uint8_t)n;
    e->ip = 0;
    for (int i = 0; i < n; i++) {
        e->addrs[i] = a[i];
        if (!e->ip && ip6_is_v4mapped(&a[i])) e->ip = ip6_to_v4(&a[i]);
    }
    e->exp = now + (uint64_t)ttl_secs * TIMER_HZ;
}

/* ---- parsing one response -------------------------------------------------- */

/* Collect every A and AAAA answer in msg[0..rlen) that is actually OURS: its
 * owner name must equal `qname`, or the target of a CNAME chain that started
 * at `qname` (bounded, DNS_MAX_CNAME hops). IPv4 records become
 * ::ffff:a.b.c.d. Returns the number newly appended, or -1 if the response is
 * not ours at all (wrong transaction id, too short to be a header, or not
 * marked as a response) -- which is a different outcome from "ours, but
 * empty": a NODATA answer for AAAA is a definitive "this name has no IPv6
 * address" and must end that half of the lookup rather than sit there
 * retransmitting. `*ttl_out` receives the smallest TTL among the records
 * accepted THIS CALL (0 if none were).
 *
 * Before this function compared owner names, ANY A/AAAA record anywhere in
 * the answer section -- for whatever name -- was accepted as the answer to
 * OUR question. DNS_NO_NAME_MATCH restores exactly that (never defined in the
 * kernel build; `make test-dns-negctl` only), so a caller of this file can
 * see, by name, which check closes that hole. */
static int collect_answers(const uint8_t *msg, int rlen, uint16_t want,
                           const char *qname, ip6_addr *out, int max, int *nout,
                           uint32_t *ttl_out)
{
    if (ttl_out) *ttl_out = 0;
    if (rlen < 12) return -1;
    /* Reject a response whose transaction ID doesn't match our query (the
     * per-query txid set by build_query_type) -- a basic guard against off-path
     * spoofed answers landing in our receive slot. */
    if (msg[0] != (uint8_t)(want >> 8) || msg[1] != (uint8_t)(want & 0xFF))
        return -1;
    if (!(msg[2] & 0x80)) return -1;            /* QR not set: not a response */

    int added = 0;
    int qdcount = (msg[4] << 8) | msg[5];
    int ancount = (msg[6] << 8) | msg[7];

    /* Skip the question section (QDCOUNT questions -- normally 1, but read the
     * real count rather than assuming). */
    int off = 12;
    for (int i = 0; i < qdcount; i++) {
        off = skip_name(msg, off, rlen);
        off += 4;                               /* QTYPE + QCLASS */
        if (off > rlen) return -1;
    }

    /* The name every accepted record's OWNER must equal. Starts as the
     * question we asked; a matching CNAME updates it to the CNAME's target,
     * so a later record in the same response can match the NEW name. */
    char target[DNS_NAME_MAX];
    canon_name(target, sizeof target, qname);
    int cnames_followed = 0;
    uint32_t min_ttl = 0;
    int have_ttl = 0;

    for (int a = 0; a < ancount && off < rlen; a++) {
        char owner[DNS_NAME_MAX];
        int noff = read_name(msg, off, rlen, owner, sizeof owner);
        if (noff < 0 || noff + 10 > rlen) break;    /* malformed owner name: stop here, keep what we have */
        off = noff;
        int type = (msg[off] << 8) | msg[off + 1];
        uint32_t ttl = ((uint32_t)msg[off + 4] << 24) | ((uint32_t)msg[off + 5] << 16) |
                       ((uint32_t)msg[off + 6] << 8) | msg[off + 7];
        int rdlen = (msg[off + 8] << 8) | msg[off + 9];
        int rdata = off + 10;
        if (rdata + rdlen > rlen) break;            /* truncated record */

#ifdef DNS_NO_NAME_MATCH
        /* NEGATIVE CONTROL ONLY (`make test-dns-negctl`), never in the kernel
         * build: accept a record regardless of whose name it is for -- the
         * bug this file used to have. tests/unit/dns_test.c's
         * unrelated-name case must FAIL with this defined. */
        int owner_ok = 1;
#else
        int owner_ok = (strcmp(owner, target) == 0);
#endif

        if (owner_ok && type == DNS_T_CNAME) {
            char cn[DNS_NAME_MAX];
            int coff = read_name(msg, rdata, rlen, cn, sizeof cn);
            if (coff >= 0 && cnames_followed < DNS_MAX_CNAME) {
                cnames_followed++;
                int j = 0;
                while (cn[j] && j < DNS_NAME_MAX - 1) { target[j] = cn[j]; j++; }
                target[j] = 0;
            }
            /* Too many CNAMEs, or an unparsable target: stop following (but
             * keep scanning -- a later record still matching `target` as it
             * stood is correctly accepted; one that doesn't is correctly
             * ignored, which is the common case once the chain is cut). */
        } else if (owner_ok && type == DNS_T_A && rdlen == 4 && *nout < max) {
            ip6_from_v4(IPV4(msg[rdata], msg[rdata + 1],
                             msg[rdata + 2], msg[rdata + 3]), &out[*nout]);
            (*nout)++; added++;
            if (!have_ttl || ttl < min_ttl) { min_ttl = ttl; have_ttl = 1; }
        } else if (owner_ok && type == DNS_T_AAAA && rdlen == 16 && *nout < max) {
            /* A AAAA record that is not a usable unicast address is a
             * misconfiguration at best and a redirection primitive at worst:
             * a name resolving to ::1 or to a multicast group must not be
             * connected to. */
            ip6_addr v;
            for (int i = 0; i < 16; i++) v.b[i] = msg[rdata + i];
            if (!ip6_is_multicast(&v) && !ip6_is_loopback(&v) &&
                !ip6_is_unspecified(&v) && !ip6_is_v4mapped(&v)) {
                out[*nout] = v;
                (*nout)++; added++;
                if (!have_ttl || ttl < min_ttl) { min_ttl = ttl; have_ttl = 1; }
            }
        }
        off = rdata + rdlen;
    }
    if (ttl_out) *ttl_out = have_ttl ? min_ttl : 0;
    return added;
}

/* ---- shared query state ----------------------------------------------------
 *
 * ONE state machine (dq_advance, below) backs both entry points:
 *   - the async pool, dq[DNS_NQ]        -- several lookups outstanding at
 *     once, none of them waited on (dns_query_start/_result/_addrs/_free).
 *   - the legacy single-shot slot, legacy_dq -- what dns_start()/dns_result()
 *     (and therefore the blocking dns_resolve()) drive.
 * They are independent instances of the same struct; nothing here shares
 * mutable state between a pool slot and legacy_dq beyond the code that
 * advances them. */

#define DNS_NQ      8               /* concurrent pool lookups */
#define DNS_RETX    50              /* retransmit after ~500 ms */
#define DNS_GIVEUP  300             /* fail after ~3 s */

/* How long the lookup waits for the SECOND family after the first one answers.
 * RFC 8305 s3 ("Resolution Delay") puts this at 50 ms and applies it the other
 * way round -- wait for AAAA, then proceed on A. 500 ms here because the
 * resolver is a hop away over emulated hardware and because a lookup that ends
 * one family short costs a whole connection attempt to discover. It only ever
 * applies when both families were asked for, i.e. never on a v4-only network. */
#define DNS_2ND_GRACE 50

struct dns_query {
    int      used;
    int      done;                  /* 1 once the address list is final */
    int      failed;
    int      sock;                  /* UDP socket, -1 for a cache/literal hit */
    uint16_t txid;                  /* the A transaction */
    uint16_t txid6;                 /* the AAAA transaction */
    uint8_t  want6;                 /* 1 if AAAA was asked for at all */
    uint8_t  got4, got6;            /* a definitive answer arrived for each */
    uint32_t ip;                    /* first A record, for the legacy uint32 API */
    ip6_addr addrs[DNS_MAXA];
    uint8_t  naddr;
    uint32_t ttl;                   /* smallest clamped TTL seen so far, 0 = none yet */
    uint64_t t0, last, t_first;     /* t_first: when the first family answered */
    char     name[DNS_NAME_MAX];
};
static struct dns_query dq[DNS_NQ];
static struct dns_query legacy_dq;  /* backs dns_start()/dns_result() -- see the bottom of this file */

/* Random ephemeral source port (well clear of the DHCP client port 68). */
static uint16_t pick_lport(void)
{
    uint16_t r;
    kernel_random_bytes((uint8_t *)&r, sizeof r);
    return (uint16_t)(49152 + r % 10000);
}

static void dq_send(struct dns_query *q)
{
    uint8_t buf[DNS_QMSG_MAX];
    int o = build_query_type(buf, q->name, DNS_T_A, &q->txid);
    if (o < 0) { q->done = 1; q->failed = 1; return; }
    q->last = timer_ticks();
    udp_send_to(q->sock, net_cfg.dns, DNS_PORT, buf, (uint16_t)o);
    if (!q->want6) return;
    o = build_query_type(buf, q->name, DNS_T_AAAA, &q->txid6);
    if (o > 0)
        udp_send_to(q->sock, net_cfg.dns, DNS_PORT, buf, (uint16_t)o);
}

/* Parse `msg` as the answer to `q`'s A transaction (for6=0) or AAAA
 * transaction (for6=1) -- `want` is the exact txid to require, passed
 * explicitly rather than read back off `q` because the TCP-fallback path
 * (below) uses a txid of its own that has nothing to do with q->txid/txid6.
 * Folds any new addresses into `q`. This is the ONE place "the answer
 * arrived" is decided, shared by the direct UDP receive path and the
 * TCP-fallback completion. */
static void dq_accept(struct dns_query *q, const uint8_t *msg, int rlen, int for6, uint16_t want)
{
    int n = q->naddr;
    uint32_t ttl = 0;
    int got = collect_answers(msg, rlen, want, q->name, q->addrs, DNS_MAXA, &n, &ttl);
    if (got < 0) return;                    /* not ours / malformed: ignore */
    if (for6) q->got6 = 1; else q->got4 = 1;
    if (n > q->naddr) {
        q->naddr = (uint8_t)n;
        if (!q->ip)
            for (int k = 0; k < n; k++)
                if (ip6_is_v4mapped(&q->addrs[k])) { q->ip = ip6_to_v4(&q->addrs[k]); break; }
    }
    if (got > 0 && (!q->ttl || ttl < q->ttl))
        q->ttl = ttl;
    if (!q->t_first) q->t_first = timer_ticks();
}

/* ---- DNS-over-TCP fallback --------------------------------------------------
 *
 * Triggered when a UDP response comes back with TC set: the answer we just
 * received is not a partial one, it is an UNRELIABLE one (RFC 1035 s4.1.1),
 * so it is discarded rather than parsed, and the same question is re-asked
 * over TCP, which has no 512/4096-byte ceiling. DNS-over-TCP framing is one
 * big-endian 2-byte length prefix followed by the message (RFC 1035 s4.2.2);
 * tcp.c is already a full client (http.c's transport), so this is plumbing,
 * not new protocol.
 *
 * DNS_TCP_NQ concurrent attempts, not one per dq[]/legacy_dq slot: EDNS0
 * above avoids truncation for essentially every real answer, so more than a
 * couple of these in flight at once has never happened on this network in
 * practice. If every slot is busy, a family's fallback simply does not
 * start -- that family's lookup stays pending until the OWNING query's own
 * DNS_GIVEUP, exactly as if the resolver had gone silent. That is an honest
 * degrade under a pile-up nothing here has produced, not a silent success. */

#define DNS_TCP_NQ 2

struct dns_tcp_fb {
    int      used;
    struct dns_query *owner;        /* &dq[i] or &legacy_dq */
    int      for6;                  /* 0 = chasing the A answer, 1 = AAAA */
    int      id;                    /* tcp.c connection id */
    int      stage;                 /* 0 connecting, 1 sending, 2 reading length, 3 reading body */
    uint16_t txid;                  /* THIS attempt's own txid -- independent of owner->txid[6] */
    uint8_t  qbuf[DNS_QMSG_MAX + 2]; int qlen, qsent;
    uint8_t  lb[2]; int lhave;
    uint8_t  body[DNS_MSG_MAX]; int need, have;
};
static struct dns_tcp_fb tcpfb[DNS_TCP_NQ];

static int dns_tcp_find_existing(struct dns_query *q, int for6)
{
    for (int i = 0; i < DNS_TCP_NQ; i++)
        if (tcpfb[i].used && tcpfb[i].owner == q && tcpfb[i].for6 == for6)
            return i;
    return -1;
}

/* Close and free every TCP fallback attempt belonging to `q`. MUST be called
 * before `q` is freed or re-armed (dns_query_free, legacy_release): without
 * it, a fallback started against the OLD lookup can outlive it, and if the
 * slot is reused for a DIFFERENT lookup before the fallback completes, its
 * eventual response gets folded into the wrong query -- `owner` is a raw
 * pointer to the slot, not a generation-tagged handle, so "the pointer is
 * still valid" and "it still names the same lookup" are not the same fact
 * once a slot can be freed and reused. */
static void dns_tcp_abandon_owner(struct dns_query *q)
{
    for (int i = 0; i < DNS_TCP_NQ; i++)
        if (tcpfb[i].used && tcpfb[i].owner == q) {
            if (tcpfb[i].id >= 0) tcp_close(tcpfb[i].id);
            tcpfb[i].used = 0;
        }
}

static void dns_tcp_start(struct dns_query *q, int for6)
{
    if (dns_tcp_find_existing(q, for6) >= 0) return;   /* already in flight */
    int slot = -1;
    for (int i = 0; i < DNS_TCP_NQ; i++) if (!tcpfb[i].used) { slot = i; break; }
    if (slot < 0) return;                              /* see the header comment above */

    struct dns_tcp_fb *f = &tcpfb[slot];
    for (unsigned i = 0; i < sizeof *f; i++) ((uint8_t *)f)[i] = 0;
    f->owner = q;
    f->for6 = for6;

    int qtype = for6 ? DNS_T_AAAA : DNS_T_A;
    int o = build_query_type(f->qbuf + 2, q->name, qtype, &f->txid);
    if (o < 0) return;                                 /* f->used stays 0 */
    f->qbuf[0] = (uint8_t)(o >> 8);
    f->qbuf[1] = (uint8_t)o;
    f->qlen = o + 2;

    f->id = tcp_connect_start(net_cfg.dns, DNS_PORT);
    if (f->id < 0) return;                             /* f->used stays 0 */
    f->stage = 0;
    f->used = 1;
}

/* Advance every TCP fallback attempt a little. Called once per dns_poll(). */
static void dns_tcp_pump(void)
{
    for (int i = 0; i < DNS_TCP_NQ; i++) {
        struct dns_tcp_fb *f = &tcpfb[i];
        if (!f->used) continue;
        if (!f->owner->used || f->owner->done) {
            /* The lookup this was chasing finished (or was abandoned)
             * through some other path while we were mid-flight -- e.g. the
             * OTHER family answered and the grace period elapsed. Nothing
             * left to deliver this to. */
            if (f->id >= 0) tcp_close(f->id);
            f->used = 0;
            continue;
        }
        switch (f->stage) {
        case 0: {                                       /* connecting */
            int st = tcp_connect_status(f->id);
            if (st < 0) { tcp_close(f->id); f->used = 0; break; }
            if (st == 1) { f->stage = 1; f->qsent = 0; }
            break;
        }
        case 1: {                                       /* sending the length-prefixed query */
            int n = tcp_send_nb(f->id, f->qbuf + f->qsent, f->qlen - f->qsent);
            if (n < 0) { tcp_close(f->id); f->used = 0; break; }
            f->qsent += n;
            if (f->qsent >= f->qlen) { f->stage = 2; f->lhave = 0; }
            break;
        }
        case 2: {                                       /* the 2-byte length prefix */
            int n = tcp_recv(f->id, f->lb + f->lhave, 2 - f->lhave);
            if (n < 0) { tcp_close(f->id); f->used = 0; break; }
            f->lhave += n;
            if (f->lhave == 2) {
                f->need = (f->lb[0] << 8) | f->lb[1];
                if (f->need == 0 || f->need > (int)sizeof f->body) {
                    /* Refused, not truncated further: a DNS-over-TCP answer
                     * this tree would ever ask about is a handful of A/AAAA
                     * records, nowhere near DNS_MSG_MAX. A resolver sending
                     * more is broken or hostile, and keeping only the first
                     * DNS_MSG_MAX bytes would silently parse a truncated
                     * MIDDLE of a message as if it were whole -- worse than
                     * refusing outright. */
                    tcp_close(f->id); f->used = 0; break;
                }
                f->stage = 3; f->have = 0;
            }
            break;
        }
        case 3: {                                       /* the body */
            int n = tcp_recv(f->id, f->body + f->have, f->need - f->have);
            if (n < 0) { tcp_close(f->id); f->used = 0; break; }
            f->have += n;
            if (f->have >= f->need) {
                dq_accept(f->owner, f->body, f->need, f->for6, f->txid);
                tcp_close(f->id);
                f->used = 0;
            }
            break;
        }
        }
    }
}

/* ---- advancing one query ---------------------------------------------------
 *
 * Receive whatever is queued, retransmit, or finalize -- shared by every
 * pool slot and by legacy_dq. Reachable only from dns_poll(). */

static uint8_t poll_buf[DNS_MSG_MAX];   /* scratch: dns_poll() runs single-threaded, one query at a time */

static void dq_advance(struct dns_query *q)
{
    uint64_t now = timer_ticks();
    if (!q->used || q->done || q->sock < 0) return;

    uint32_t src; uint16_t sport;
    int rlen;
    int died = 0;
    /* Drain: with two transactions in flight on one socket, the A and AAAA
     * responses are usually both already queued by the time we look. */
    while ((rlen = udp_recv(q->sock, poll_buf, sizeof poll_buf, &src, &sport)) != 0) {
        if (rlen < 0) {                     /* ICMP error against our port */
            q->done = 1; q->failed = 1;
            died = 1;
            break;
        }
        if (src != net_cfg.dns || sport != DNS_PORT)
            continue;                       /* someone else's datagram */
        if (rlen < 12) continue;            /* not even a header */

        int is_a = (poll_buf[0] == (uint8_t)(q->txid >> 8) && poll_buf[1] == (uint8_t)(q->txid & 0xFF));
        int is_6 = q->want6 && (poll_buf[0] == (uint8_t)(q->txid6 >> 8) && poll_buf[1] == (uint8_t)(q->txid6 & 0xFF));
        if (!is_a && !is_6)
            continue;                       /* neither transaction: spoofed or stale, ignore */

        if (poll_buf[2] & 0x02) {           /* TC: truncated, not a usable answer */
            dns_tcp_start(q, is_a ? 0 : 1);
            continue;
        }
        dq_accept(q, poll_buf, rlen, is_a ? 0 : 1, is_a ? q->txid : q->txid6);
    }
    if (died) return;

    /* Finished when both families have answered -- or when one has and the
     * other has had its grace period. A name with an A record and a slow
     * (or filtered) AAAA must not stall the connection behind it. */
    int both = q->got4 && (!q->want6 || q->got6);
    int graced = q->t_first && (now - q->t_first >= DNS_2ND_GRACE);
    if (both || (q->t_first && graced)) {
        q->done = 1;
        q->failed = (q->naddr == 0);
        if (q->naddr) dns_cache_put_set(q->name, q->addrs, q->naddr, q->ttl ? q->ttl : DNS_TTL_FLOOR);
        return;
    }
    if (now - q->t0 >= DNS_GIVEUP) {
        /* Out of time. Whatever arrived is still usable: a lookup that got
         * an A record and never heard about AAAA has an answer. */
        q->done = 1;
        q->failed = (q->naddr == 0);
        if (q->naddr) dns_cache_put_set(q->name, q->addrs, q->naddr, q->ttl ? q->ttl : DNS_TTL_FLOOR);
        return;
    }
    if (now - q->last >= DNS_RETX) dq_send(q);
}

/* ---- async resolver pool (the non-blocking socket path) --------------------
 *
 * Several lookups outstanding at once, each advanced a little on every
 * dns_poll() and never waited on -- what sock.c needs. The slots are
 * independent (own UDP socket, own transaction ids, own timers). */

/* Start a lookup. Returns a query id (>= 0) the caller polls with
 * dns_query_result() and MUST release with dns_query_free(), or -1 if the name
 * is too long or every slot is busy. An IP literal and a cache hit both return
 * an already-complete slot, so the caller's state machine has one shape. */
int dns_query_start(const char *name)
{
    if (!name || !*name) return -1;
    int n = 0; while (name[n]) n++;
    if (n >= DNS_NAME_MAX) return -1;

    int id = -1;
    for (int i = 0; i < DNS_NQ; i++) if (!dq[i].used) { id = i; break; }
    if (id < 0) return -1;
    struct dns_query *q = &dq[id];
    for (unsigned i = 0; i < sizeof *q; i++) ((uint8_t *)q)[i] = 0;
    q->used = 1;
    q->sock = -1;
    name_copy(q->name, name);

    /* An IPv6 literal ("2001:db8::2", and the bracketless form a URL parser
     * hands over) resolves to itself, exactly as a dotted quad does. */
    ip6_addr lit6;
    if (ip6_parse(name, &lit6) && !ip6_is_multicast(&lit6)) {
        q->addrs[0] = lit6;
        q->naddr = 1;
        q->ip = ip6_to_v4(&lit6);       /* 0 for a real v6 literal */
        q->done = 1;
        return id;
    }

    uint32_t fast = parse_ip_literal(name);
    if (fast) {
        ip6_from_v4(fast, &q->addrs[0]);
        q->naddr = 1;
        q->ip = fast;
        q->done = 1;
        return id;
    }
    struct dns_cent *c = dns_cache_find(name);
    if (c) {
        for (int i = 0; i < c->naddr; i++) q->addrs[i] = c->addrs[i];
        q->naddr = c->naddr;
        q->ip = c->ip;
        q->done = 1;
        return id;
    }

    /* Ask for AAAA only when IPv6 could actually be used: a routable address
     * that has passed DAD, and a default router. Without that, a AAAA answer
     * would only produce destinations we cannot reach -- and on a v4-only
     * network this keeps the wire byte-identical to before IPv6 existed. */
#ifdef IP6_NEGCTL_ALWAYS_AAAA
    /* NEGATIVE CONTROL ONLY (`make test-ip6-dns-negctl`), never in the kernel
     * build: ask for AAAA unconditionally -- the obvious way to write this, and
     * the one that puts a second query on the wire of every v4-only network for
     * answers it could never use. tests/unit/ip6_dns_test.c must FAIL with this
     * defined, because "the v4 wire is unchanged" is a claim about a byte count
     * and nothing else can check it. */
    q->want6 = 1;
#else
    q->want6 = (ip6_up() == 2);
#endif

    for (int tries = 0; tries < 16 && q->sock < 0; tries++)
        q->sock = udp_bind(pick_lport());
    if (q->sock < 0) { q->used = 0; return -1; }
    /* Kick the resolver's ARP without waiting for it: arp_warm() blocks, and
     * nothing on this path may. A cold cache costs one DNS retransmit. */
    { uint8_t mac[ETH_ALEN]; arp_resolve(net_cfg.dns, mac); }
    q->t0 = timer_ticks();
    dq_send(q);
    return id;
}

/* 0 = still pending, 0xFFFFFFFF = failed/timed out, else the IPv4 address.
 * Non-destructive: the slot lives until dns_query_free().
 *
 * A name that resolved ONLY to IPv6 addresses reports 0xFFFFFFFF here: there is
 * no IPv4 address to return and pretending otherwise would be worse. Callers
 * that can speak both families use dns_query_addrs() instead. */
uint32_t dns_query_result(int id)
{
    if (id < 0 || id >= DNS_NQ || !dq[id].used) return 0xFFFFFFFFu;
    struct dns_query *q = &dq[id];
    if (!q->done) return 0;
    if (q->failed || !q->ip) return 0xFFFFFFFFu;
    return q->ip;
}

/* 0 = still pending, < 0 = failed, else the number of destinations written to
 * `out`, MOST PREFERRED FIRST.
 *
 * The ordering is RFC 6724 section 6, evaluated here rather than at answer time
 * because it depends on which of our own addresses are usable RIGHT NOW -- an
 * address that has just been deprecated, or a router whose lifetime expired,
 * changes the answer without the DNS records changing at all. */
int dns_query_addrs(int id, ip6_addr *out, int max)
{
    if (id < 0 || id >= DNS_NQ || !dq[id].used || max <= 0) return -1;
    struct dns_query *q = &dq[id];
    if (!q->done) return 0;
    if (q->failed || q->naddr == 0) return -1;

    int n = q->naddr;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = q->addrs[i];

    struct ip6_src_cand cand[IP6_NADDR + 1];
    int nc = ip6_dual_candidates(cand, IP6_NADDR + 1);
    ip6_sort_dests(out, n, cand, nc);
    return n;
}

void dns_query_free(int id)
{
    if (id < 0 || id >= DNS_NQ || !dq[id].used) return;
    dns_tcp_abandon_owner(&dq[id]);
    if (dq[id].sock >= 0) udp_close(dq[id].sock);
    dq[id].sock = -1;
    dq[id].used = 0;
}

/* Pumped from net_poll(): receive answers, retransmit, time out, and advance
 * any TCP fallback in flight. Every slot (and legacy_dq) advances on every
 * call, which is what makes the pool lookups concurrent. */
void dns_poll(void)
{
    for (int i = 0; i < DNS_NQ; i++) dq_advance(&dq[i]);
    dq_advance(&legacy_dq);
    dns_tcp_pump();
}

/* ---- the legacy single-shot API --------------------------------------------
 *
 * dns_start()/dns_result() predate the pool above and are kept as their own
 * documented entry point (SYS_NET_DNS/SYS_NET_DNS_RESULT, and dns_resolve()'s
 * blocking wrapper for the SYS_HTTP_GET path) rather than folded into it,
 * because of ONE deliberate behavioural difference: this path never reads
 * the name cache. dns_resolve() is what every existing https test rides, and
 * a cache hit here would change what a fetch does on a stale record in a way
 * nothing asked for. Everything else -- name matching, CNAME chasing, EDNS0,
 * the TCP fallback, real TTLs -- is the SAME code as the pool, via
 * dq_advance()/dns_poll(), not a second copy of it. */

static void legacy_release(void)
{
    dns_tcp_abandon_owner(&legacy_dq);
    if (legacy_dq.sock >= 0) udp_close(legacy_dq.sock);
    legacy_dq.sock = -1;
    legacy_dq.used = 0;
}

/* Non-blocking: send the query, arm the receive socket, record the start time. */
void dns_start(const char *name)
{
    if (legacy_dq.used) legacy_release();       /* a lookup was already in flight: abandon it */
    for (unsigned i = 0; i < sizeof legacy_dq; i++) ((uint8_t *)&legacy_dq)[i] = 0;
    legacy_dq.used = 1;
    legacy_dq.sock = -1;                         /* zero is a VALID socket index; the bind loop
                                                     below only runs while sock is negative */
    name_copy(legacy_dq.name, name);

    uint32_t fast = parse_ip_literal(name);
    if (fast) {
        ip6_from_v4(fast, &legacy_dq.addrs[0]);
        legacy_dq.naddr = 1;
        legacy_dq.ip = fast;
        legacy_dq.done = 1;
        return;
    }

    /* IPv4-only on purpose: this is the legacy uint32 entry point (the
     * Network app's diagnostic, and dns_resolve()'s transport), and asking
     * for AAAA here would put a second datagram on the wire for an answer
     * nothing reads. dns_query_start() is where dual-stack lookups belong. */
    legacy_dq.want6 = 0;

    for (int tries = 0; tries < 16 && legacy_dq.sock < 0; tries++)
        legacy_dq.sock = udp_bind(pick_lport());
    if (legacy_dq.sock < 0) { legacy_dq.used = 0; return; }   /* no free socket: dns_result fails */
    { uint8_t mac[ETH_ALEN]; arp_resolve(net_cfg.dns, mac); }
    legacy_dq.t0 = timer_ticks();
    dq_send(&legacy_dq);
}

/* 0 = pending, 0xFFFFFFFF = timeout/ICMP error/no socket, else the resolved
 * IP (host order). A terminal answer releases the socket.
 *
 * Self-pumping (calls dns_poll()): the ORIGINAL implementation did its own
 * udp_recv() inline, so a caller that is not itself part of a net_poll() loop
 * (a syscall handler polled directly by userland, or a host unit test with no
 * background pump at all) still saw progress on every call. Folding the
 * receive/retransmit/finalize logic into dq_advance()/dns_poll() must not
 * lose that property -- in the real kernel dns_poll() is ALSO reached every
 * frame via the WM loop's net_poll(), so this is a second, redundant call
 * there, not a second mechanism; dq_advance() is cheap to call twice in one
 * tick (retransmit is time-gated, receive just drains whatever is queued). */
uint32_t dns_result(void)
{
    dns_poll();
    if (!legacy_dq.used) return 0xFFFFFFFFu;
    if (!legacy_dq.done) return 0;
    uint32_t rc = (legacy_dq.failed || !legacy_dq.ip) ? 0xFFFFFFFFu : legacy_dq.ip;
    legacy_release();
    return rc;
}

uint32_t dns_resolve(const char *name)
{
    uint32_t lit = parse_ip_literal(name);
    if (lit) return lit;
    arp_warm(net_cfg.dns, 30);                /* resolve the resolver's MAC first, so the */
    dns_start(name);                          /* query isn't dropped on a cold ARP cache */
    uint64_t start = timer_ticks();
    /* dq_advance()'s own DNS_GIVEUP is the real deadline (retransmission
     * included -- dns_result() pumps dns_poll() every call, which retransmits
     * every DNS_RETX ticks on its own now). This loop only needs to run long
     * enough to observe that deadline fire, plus a little slack for
     * scheduling jitter -- not a SECOND independent timeout to keep in sync
     * with the first, which is what the old code had (two 300-tick constants
     * that happened to agree because nothing had yet made them disagree). */
    while (timer_ticks() - start < DNS_GIVEUP + TIMER_HZ) {
        net_poll();
        uint32_t r = dns_result();
        if (r == 0xFFFFFFFFu) return 0;
        if (r) return r;                      /* dq_advance already cached it; dns_result already freed the slot */
        net_idle();                                  /* sleep to the next tick; don't peg the host CPU */
    }
    legacy_release();                          /* outer bound reached first: don't leak the socket */
    return 0;
}
