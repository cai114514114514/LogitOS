/* IPv6 addresses as pure logic: text form, classification, and the whole of
 * RFC 6724 (source-address selection and destination ordering).
 *
 * Nothing here touches the network, a timer, or a lock, which is deliberate:
 * this is the half of an IPv6 stack that is all rules and no I/O, so it is the
 * half that can be exhaustively unit-tested on the host. tests/unit/ip6_addr_test.c
 * drives every function below against a table of cases, including the ones a
 * stack usually gets wrong -- "::" that expands to zero groups, an embedded
 * IPv4 quad, the RFC 5952 tie-break when two zero runs are the same length,
 * and the precedence comparison that decides whether a dual-stack host prefers
 * v6 or v4 for a given destination. */

#include <stdint.h>
#include "ip6.h"

/* Freestanding: the kernel's string.c provides these, but this file is also
 * compiled standalone into the host tests, so it uses its own. */
static void a_copy(uint8_t *d, const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) d[i] = s[i];
}

/* ---- classification ----------------------------------------------------- */

int ip6_equal(const ip6_addr *a, const ip6_addr *b)
{
    for (int i = 0; i < 16; i++)
        if (a->b[i] != b->b[i]) return 0;
    return 1;
}

int ip6_is_unspecified(const ip6_addr *a)
{
    for (int i = 0; i < 16; i++)
        if (a->b[i]) return 0;
    return 1;
}

int ip6_is_loopback(const ip6_addr *a)
{
    for (int i = 0; i < 15; i++)
        if (a->b[i]) return 0;
    return a->b[15] == 1;
}

int ip6_is_multicast(const ip6_addr *a) { return a->b[0] == 0xFF; }

int ip6_is_linklocal(const ip6_addr *a)
{
    return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80;
}

int ip6_is_v4mapped(const ip6_addr *a)
{
    for (int i = 0; i < 10; i++)
        if (a->b[i]) return 0;
    return a->b[10] == 0xFF && a->b[11] == 0xFF;
}

/* RFC 4007 scope, as RFC 6724 s3.1/3.2 define it for the selection rules.
 * The two that surprise people: a Unique Local Address (fc00::/7) has GLOBAL
 * scope -- it is not "site-local" in the RFC 6724 sense -- while the deprecated
 * site-local prefix fec0::/10 really does have site scope, and QEMU's SLIRP
 * hands out exactly that prefix by default. */
int ip6_scope(const ip6_addr *a)
{
    if (ip6_is_multicast(a))
        return a->b[1] & 0x0F;
    if (ip6_is_linklocal(a) || ip6_is_loopback(a))
        return IP6_SCOPE_LINK;
    if (a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0xC0)     /* fec0::/10 */
        return IP6_SCOPE_SITE;
    if (ip6_is_v4mapped(a)) {
        /* RFC 6724 s3.2: 169.254/16 and 127/8 are link-local; every other IPv4
         * address (private ranges included) is global. */
        if (a->b[12] == 127) return IP6_SCOPE_LINK;
        if (a->b[12] == 169 && a->b[13] == 254) return IP6_SCOPE_LINK;
        return IP6_SCOPE_GLOBAL;
    }
    return IP6_SCOPE_GLOBAL;
}

int ip6_common_prefix_len(const ip6_addr *a, const ip6_addr *b)
{
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t x = (uint8_t)(a->b[i] ^ b->b[i]);
        if (!x) { n += 8; continue; }
        for (int k = 7; k >= 0; k--) {
            if (x & (1u << k)) return n;
            n++;
        }
        return n;
    }
    return n;
}

int ip6_prefix_eq(const ip6_addr *a, const ip6_addr *b, int plen)
{
    if (plen < 0) plen = 0;
    if (plen > 128) plen = 128;
    int whole = plen / 8, bits = plen % 8;
    for (int i = 0; i < whole; i++)
        if (a->b[i] != b->b[i]) return 0;
    if (bits) {
        uint8_t m = (uint8_t)(0xFF << (8 - bits));
        if ((a->b[whole] & m) != (b->b[whole] & m)) return 0;
    }
    return 1;
}

void ip6_from_v4(uint32_t v4, ip6_addr *out)
{
    for (int i = 0; i < 10; i++) out->b[i] = 0;
    out->b[10] = 0xFF; out->b[11] = 0xFF;
    out->b[12] = (uint8_t)(v4 >> 24);
    out->b[13] = (uint8_t)(v4 >> 16);
    out->b[14] = (uint8_t)(v4 >> 8);
    out->b[15] = (uint8_t)v4;
}

uint32_t ip6_to_v4(const ip6_addr *a)
{
    if (!ip6_is_v4mapped(a)) return 0;
    return ((uint32_t)a->b[12] << 24) | ((uint32_t)a->b[13] << 16) |
           ((uint32_t)a->b[14] << 8) | a->b[15];
}

void ip6_multicast_mac(const ip6_addr *a, uint8_t mac[ETH_ALEN])
{
    mac[0] = 0x33; mac[1] = 0x33;
    mac[2] = a->b[12]; mac[3] = a->b[13]; mac[4] = a->b[14]; mac[5] = a->b[15];
}

void ip6_solicited_node(const ip6_addr *a, ip6_addr *out)
{
    static const uint8_t pre[13] = { 0xFF, 0x02, 0,0,0,0,0,0, 0,0, 0,0x01, 0xFF };
    a_copy(out->b, pre, 13);
    out->b[13] = a->b[13];
    out->b[14] = a->b[14];
    out->b[15] = a->b[15];
}

void ip6_eui64_into(const uint8_t mac[ETH_ALEN], ip6_addr *a)
{
    a->b[8]  = (uint8_t)(mac[0] ^ 0x02);    /* flip the universal/local bit */
    a->b[9]  = mac[1];
    a->b[10] = mac[2];
    a->b[11] = 0xFF;
    a->b[12] = 0xFE;
    a->b[13] = mac[3];
    a->b[14] = mac[4];
    a->b[15] = mac[5];
}

void ip6_linklocal_from_mac(const uint8_t mac[ETH_ALEN], ip6_addr *out)
{
    for (int i = 0; i < 16; i++) out->b[i] = 0;
    out->b[0] = 0xFE; out->b[1] = 0x80;
    ip6_eui64_into(mac, out);
}

/* ---- text: parse -------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "a.b.c.d" at *sp, consuming it. 1 on success. */
static int parse_v4_tail(const char **sp, uint8_t out[4])
{
    const char *s = *sp;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, nd = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            if (++nd > 3 || v > 255) return 0;
        }
        /* "01" and "0.0.0.00" are rejected: a leading zero in a dotted quad is
         * historically octal, and accepting both readings is how parsers get
         * turned into filter bypasses. */
        if (nd > 1 && *(s - nd) == '0') return 0;
        out[part] = (uint8_t)v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    *sp = s;
    return 1;
}

int ip6_parse(const char *s, ip6_addr *out)
{
    uint8_t buf[16];
    int head = 0;               /* bytes written before "::" */
    uint8_t tail[16];
    int ntail = 0;              /* bytes written after "::" */
    int gap = 0;                /* seen "::" */
    uint8_t *cur = buf;
    int *ncur = &head;

    if (!s || !*s) return 0;

    if (s[0] == ':') {
        if (s[1] != ':') return 0;      /* a lone leading ':' is not legal */
        s += 2;
        gap = 1;
        cur = tail; ncur = &ntail;
        if (!*s) { for (int i = 0; i < 16; i++) out->b[i] = 0; return 1; }
    }

    for (;;) {
        /* A group is 1..4 hex digits, or the tail of the address written as a
         * dotted quad ("::ffff:10.0.2.2"). Try the quad first: it is only a
         * quad if a '.' follows the digits. */
        const char *probe = s;
        int isquad = 0;
        while ((*probe >= '0' && *probe <= '9')) probe++;
        if (*probe == '.') isquad = 1;

        if (isquad) {
            uint8_t q[4];
            if (!parse_v4_tail(&s, q)) return 0;
            if (*ncur + 4 > 16) return 0;
            for (int i = 0; i < 4; i++) cur[(*ncur)++] = q[i];
            break;                       /* a quad is always the last element */
        }

        int v = 0, nd = 0;
        while (hexval(*s) >= 0) {
            v = (v << 4) | hexval(*s++);
            if (++nd > 4) return 0;
        }
        if (nd == 0) return 0;
        if (*ncur + 2 > 16) return 0;
        cur[(*ncur)++] = (uint8_t)(v >> 8);
        cur[(*ncur)++] = (uint8_t)v;

        if (*s != ':') break;
        s++;
        if (*s == ':') {
            if (gap) return 0;           /* only one "::" */
            gap = 1;
            s++;
            cur = tail; ncur = &ntail;
            if (!*s) break;              /* trailing "::" */
            continue;
        }
        if (!*s) return 0;               /* trailing single ':' */
    }

    if (*s) return 0;                    /* trailing junk (incl. a %scope-id) */

    if (gap) {
        /* "::" must stand for AT LEAST one zero group -- "1:2:3:4:5:6:7::8" is
         * not a legal spelling of a full address. */
        if (head + ntail > 14) return 0;
        for (int i = 0; i < head; i++) out->b[i] = buf[i];
        for (int i = head; i < 16 - ntail; i++) out->b[i] = 0;
        for (int i = 0; i < ntail; i++) out->b[16 - ntail + i] = tail[i];
    } else {
        if (head != 16) return 0;
        for (int i = 0; i < 16; i++) out->b[i] = buf[i];
    }
    return 1;
}

/* ---- text: format (RFC 5952) -------------------------------------------- */

static int put(char *out, int max, int o, char c)
{
    if (o < max - 1) out[o] = c;
    return o + 1;
}

static int put_dec(char *out, int max, int o, unsigned v)
{
    char t[4]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) o = put(out, max, o, t[--i]);
    return o;
}

int ip6_format(const ip6_addr *a, char *out, int max)
{
    if (max < 1) return 0;
    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (uint16_t)((a->b[2 * i] << 8) | a->b[2 * i + 1]);

    /* RFC 5952 s5: IPv4-mapped keeps the dotted form. The unspecified address
     * and the loopback are handled by the generic path ("::" / "::1"). */
    int v4tail = ip6_is_v4mapped(a) ? 12 : 0;

    /* Longest run of zero groups, at least two long; leftmost wins a tie
     * (RFC 5952 s4.2.3). Runs inside the IPv4 tail are not candidates. */
    int ngroups = v4tail ? 6 : 8;
    int best = -1, bestlen = 0;
    for (int i = 0; i < ngroups;) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < ngroups && !g[j]) j++;
        if (j - i > bestlen) { bestlen = j - i; best = i; }
        i = j;
    }
    if (bestlen < 2) best = -1;

    static const char hex[] = "0123456789abcdef";
    int o = 0;
    /* need_sep tracks whether a ':' is owed before the next element. The "::"
     * writes two colons and clears it: the second colon IS the separator for
     * whatever follows, which is the detail that otherwise yields ":::". */
    int need_sep = 0;
    for (int i = 0; i < ngroups;) {
        if (i == best) {
            o = put(out, max, o, ':');
            o = put(out, max, o, ':');
            i += bestlen;
            need_sep = 0;
            continue;
        }
        if (need_sep) o = put(out, max, o, ':');
        uint16_t v = g[i];
        int started = 0;
        for (int sh = 12; sh >= 0; sh -= 4) {
            int d = (v >> sh) & 0xF;
            if (d || started || sh == 0) { o = put(out, max, o, hex[d]); started = 1; }
        }
        need_sep = 1;
        i++;
    }
    if (v4tail) {
        if (need_sep) o = put(out, max, o, ':');
        for (int i = 0; i < 4; i++) {
            if (i) o = put(out, max, o, '.');
            o = put_dec(out, max, o, a->b[12 + i]);
        }
    }
    if (o >= max) { out[0] = 0; return 0; }
    out[o] = 0;
    return o;
}

/* ---- RFC 6724 policy table ---------------------------------------------- */

/* The default table from RFC 6724 s2.1, longest-matching-prefix lookup. The
 * order matters only for readability -- the search takes the longest match. */
struct policy { uint8_t pre[16]; uint8_t plen, precedence, label; };

static const struct policy policies[] = {
    /* ::1/128 */
    { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 },                  128, 50,  0 },
    /* ::/0 */
    { { 0 },                                                  0, 40,  1 },
    /* ::ffff:0:0/96 */
    { { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF },                     96, 35,  4 },
    /* 2002::/16 (6to4) */
    { { 0x20,0x02 },                                         16, 30,  2 },
    /* 2001::/32 (Teredo) */
    { { 0x20,0x01,0,0 },                                     32,  5,  5 },
    /* fc00::/7 (ULA) */
    { { 0xFC },                                               7,  3, 13 },
    /* ::/96 (IPv4-compatible, deprecated) */
    { { 0 },                                                 96,  1,  3 },
    /* fec0::/10 (site-local, deprecated -- and what SLIRP hands out) */
    { { 0xFE,0xC0 },                                         10,  1, 11 },
    /* 3ffe::/16 (6bone, returned) */
    { { 0x3F,0xFE },                                         16,  1, 12 },
};

static const struct policy *policy_for(const ip6_addr *a)
{
    const struct policy *best = &policies[1];      /* ::/0 always matches */
    for (unsigned i = 0; i < sizeof policies / sizeof policies[0]; i++) {
        const struct policy *p = &policies[i];
        ip6_addr pre;
        a_copy(pre.b, p->pre, 16);
        if (ip6_prefix_eq(a, &pre, p->plen) && p->plen >= best->plen)
            best = p;
    }
    return best;
}

int ip6_policy_precedence(const ip6_addr *a) { return policy_for(a)->precedence; }
int ip6_policy_label(const ip6_addr *a)      { return policy_for(a)->label; }

/* ---- RFC 6724 s5: source address selection ------------------------------ */

/* Compare candidates a and b as sources for dst. > 0 if a is preferred. */
static int src_better(const struct ip6_src_cand *a, const struct ip6_src_cand *b,
                      const ip6_addr *dst)
{
    /* Rule 1: prefer the same address. */
    int sa = ip6_equal(&a->addr, dst), sb = ip6_equal(&b->addr, dst);
    if (sa != sb) return sa - sb;

    /* Rule 2: prefer appropriate scope. The comparison is not "closest scope"
     * -- it is: of two candidates, if the smaller-scoped one is too small for
     * the destination, take the larger; otherwise take the smaller. */
    int za = ip6_scope(&a->addr), zb = ip6_scope(&b->addr), zd = ip6_scope(dst);
    if (za != zb) {
        if (za < zb) return (za < zd) ? -1 : 1;
        return (zb < zd) ? 1 : -1;
    }

    /* Rule 3: avoid deprecated addresses. */
    int da = (a->state == IP6_DEPRECATED), db = (b->state == IP6_DEPRECATED);
    if (da != db) return db - da;

    /* Rules 4 (home address), 5 (outgoing interface), 5.5 (next-hop prefix) and
     * 7 (temporary addresses) do not apply: no Mobile IPv6, one interface, no
     * RFC 4941 privacy addresses. */

    /* Rule 6: prefer matching label. */
    int la = ip6_policy_label(&a->addr) == ip6_policy_label(dst);
    int lb = ip6_policy_label(&b->addr) == ip6_policy_label(dst);
    if (la != lb) return la - lb;

    /* Rule 8: longest matching prefix. Compared over the prefix the address was
     * configured with, per RFC 6724's note that bits beyond it say nothing. */
    int ca = ip6_common_prefix_len(&a->addr, dst);
    int cb = ip6_common_prefix_len(&b->addr, dst);
    if (ca > a->plen) ca = a->plen;
    if (cb > b->plen) cb = b->plen;
    if (ca != cb) return ca - cb;
    return 0;
}

int ip6_select_source(const ip6_addr *dst,
                      const struct ip6_src_cand *cand, int ncand)
{
    int best = -1;
    for (int i = 0; i < ncand; i++) {
        /* A tentative address has not passed DAD and MUST NOT be used
         * (RFC 4862 s5.4). This is the check that keeps a duplicate address
         * off the wire, so it is here and not only at the caller. */
        if (cand[i].state == IP6_TENTATIVE) continue;
        /* A v4-mapped candidate can only source a v4-mapped destination, and
         * vice versa -- the families do not interoperate at the IP layer. */
        if (ip6_is_v4mapped(&cand[i].addr) != ip6_is_v4mapped(dst)) continue;
        /* Never source from a multicast or unspecified address. */
        if (ip6_is_multicast(&cand[i].addr) || ip6_is_unspecified(&cand[i].addr))
            continue;
        /* A source of SMALLER scope than the destination is not merely a poor
         * choice, it is unusable: a reply to a link-local source cannot leave
         * the link. RFC 6724 s5 leaves this to rule 2's ordering, but ordering
         * alone would hand back fe80:: for a global destination when that is
         * all we have. Excluding it is what makes ip6_select_source() return -1
         * for "IPv6 is configured but cannot reach the Internet" -- which is in
         * turn what makes destination-ordering rule 1 demote such a destination
         * below IPv4 instead of stranding the connection. Same behaviour as
         * Linux returning EADDRNOTAVAIL. */
#ifndef IP6_NEGCTL_NO_SCOPE_GUARD
        if (ip6_scope(&cand[i].addr) < ip6_scope(dst)) continue;
#endif  /* the guard is removed only by `make test-ip6-negctl`, which requires
         * the suite to FAIL without it. Never defined by the kernel build. */
        if (best < 0 || src_better(&cand[i], &cand[best], dst) > 0)
            best = i;
    }
    return best;
}

/* ---- RFC 6724 s6: destination ordering ---------------------------------- */

/* > 0 if DA should be tried before DB. `sa`/`sb` are their selected sources
 * (NULL = unusable destination). */
static int dst_better(const ip6_addr *da, const struct ip6_src_cand *sa,
                      const ip6_addr *db, const struct ip6_src_cand *sb)
{
    /* Rule 1: avoid unusable destinations. */
    if ((sa != 0) != (sb != 0)) return sa ? 1 : -1;
    if (!sa) return 0;

    /* Rule 2: prefer matching scope. */
    int ma = ip6_scope(&sa->addr) == ip6_scope(da);
    int mb = ip6_scope(&sb->addr) == ip6_scope(db);
    if (ma != mb) return ma - mb;

    /* Rule 3: avoid deprecated addresses. */
    int pa = (sa->state != IP6_DEPRECATED), pb = (sb->state != IP6_DEPRECATED);
    if (pa != pb) return pa - pb;

    /* Rule 4 (home addresses) does not apply. */

    /* Rule 5: prefer matching label. */
    int la = ip6_policy_label(&sa->addr) == ip6_policy_label(da);
    int lb = ip6_policy_label(&sb->addr) == ip6_policy_label(db);
    if (la != lb) return la - lb;

    /* Rule 6: prefer higher precedence. THIS is the rule that decides whether a
     * dual-stack host reaches for IPv6 or IPv4: a global v6 destination has
     * precedence 40 against IPv4's 35, but SLIRP's default fec0::/10 has 1, so
     * "prefers IPv6" is not a constant -- it is a property of the prefix. */
    int qa = ip6_policy_precedence(da), qb = ip6_policy_precedence(db);
    if (qa != qb) return qa - qb;

    /* Rule 7 (native over tunnelled) does not apply: no tunnels. */

    /* Rule 8: prefer smaller scope. */
    int za = ip6_scope(da), zb = ip6_scope(db);
    if (za != zb) return zb - za;

    /* Rule 9: longest matching prefix -- IPv6 pairs only.
     *
     * KNOWN DIVERGENCE from a literal reading of RFC 6724 s6, and a deliberate
     * one. The rule as written also compares two IPv4 destinations by how many
     * bits they share with our own IPv4 address, which for 10.0.2.15 makes
     * 8.8.8.8 (102 bits) outrank 1.1.1.1 (100) for no reason anyone can defend;
     * RFC 6724 s10.3 says as much. Applying it to IPv4 would also silently
     * re-order every existing v4 lookup in this OS, which is exactly the
     * regression this line must not cause. So a v4-only answer comes back in
     * the order the resolver gave it, byte for byte. */
    int fa = ip6_is_v4mapped(da), fb = ip6_is_v4mapped(db);
    if (fa == fb && !fa) {
        int ca = ip6_common_prefix_len(&sa->addr, da);
        int cb = ip6_common_prefix_len(&sb->addr, db);
        if (ca != cb) return ca - cb;
    }
    /* Rule 10: leave the order as it was. */
    return 0;
}

void ip6_sort_dests(ip6_addr *dst, int n,
                    const struct ip6_src_cand *cand, int ncand)
{
    /* Insertion sort, and it must stay a STABLE one: rule 10 says destinations
     * the nine rules cannot separate keep the resolver's order, which is how a
     * round-robin DNS answer stays round-robin. */
    for (int i = 1; i < n; i++) {
        ip6_addr key = dst[i];
        int ks = ip6_select_source(&key, cand, ncand);
        int j = i - 1;
        while (j >= 0) {
            int js = ip6_select_source(&dst[j], cand, ncand);
            if (dst_better(&key, ks >= 0 ? &cand[ks] : 0,
                           &dst[j], js >= 0 ? &cand[js] : 0) > 0) {
                dst[j + 1] = dst[j];
                j--;
            } else break;
        }
        dst[j + 1] = key;
    }
}
