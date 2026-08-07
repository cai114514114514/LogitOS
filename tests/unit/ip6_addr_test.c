/* Host unit tests for the pure half of the IPv6 stack: address text form,
 * classification, and RFC 6724 source-address selection + destination ordering.
 *
 * This is where most of the risk in an IPv6 stack actually lives. Packet
 * plumbing fails loudly; a wrong precedence comparison fails as "sometimes the
 * page does not load", months later, on someone else's network. So the RFC 6724
 * rules are driven from an explicit table of cases with the reasoning written
 * next to each one, including the two that decide whether this OS is usable on
 * a dual-stack network at all:
 *
 *   - a v6 destination with NO usable source must sort BELOW IPv4 (rule 1), or
 *     a host with a broken v6 path becomes a host that cannot reach anything;
 *   - QEMU SLIRP's default fec0::/10 has precedence 1, BELOW IPv4's 35, so a
 *     correct implementation prefers IPv4 to it. "Prefers IPv6" is a property
 *     of the prefix, not a switch.
 *
 * Built with: cc -DLOGIT_NET_HOST tests/unit/ip6_addr_test.c -Ic/net/ip -Ic/net/link */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ip6.h"
#include "ip6_addr.c"

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL(%d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ip6_addr A(const char *s)
{
    ip6_addr a;
    memset(&a, 0, sizeof a);
    if (!ip6_parse(s, &a)) { printf("FATAL: cannot parse %s\n", s); failed++; }
    return a;
}

static int EQ(const ip6_addr *a, const char *s)
{
    ip6_addr b = A(s);
    return ip6_equal(a, &b);
}

static const char *F(const ip6_addr *a)
{
    static char buf[4][64];
    static int k;
    char *o = buf[k++ & 3];
    ip6_format(a, o, 64);
    return o;
}

/* ---- text ---------------------------------------------------------------- */

static void test_parse_format(void)
{
    /* Canonical output (RFC 5952): lower case, no leading zeros, longest zero
     * run compressed, leftmost on a tie. Each input must round-trip to the
     * canonical spelling on the right. */
    static const struct { const char *in, *out; } t[] = {
        { "::",                                        "::" },
        { "::1",                                       "::1" },
        { "1::",                                       "1::" },
        { "2001:db8::1",                               "2001:db8::1" },
        { "2001:0db8:0000:0000:0000:0000:0000:0001",   "2001:db8::1" },
        { "2001:DB8::1",                               "2001:db8::1" },
        { "fe80::5054:ff:fe12:3456",                   "fe80::5054:ff:fe12:3456" },
        { "fec0::2",                                   "fec0::2" },
        { "ff02::1",                                   "ff02::1" },
        { "ff02::1:ff12:3456",                         "ff02::1:ff12:3456" },
        { "2606:2800:220:1:248:1893:25c8:1946",
          "2606:2800:220:1:248:1893:25c8:1946" },
        /* Tie-break: two zero runs of length 2, the LEFTMOST is compressed. */
        { "1:0:0:2:0:0:3:4",                           "1::2:0:0:3:4" },
        /* A longer run later beats a shorter one earlier. */
        { "2001:0:0:1:0:0:0:1",                        "2001:0:0:1::1" },
        /* A single zero group is never compressed (RFC 5952 4.2.2). */
        { "1:2:3:4:5:6:0:8",                           "1:2:3:4:5:6:0:8" },
        /* IPv4-mapped keeps the dotted tail. */
        { "::ffff:10.0.2.2",                           "::ffff:10.0.2.2" },
        { "::ffff:0:0",                                "::ffff:0.0.0.0" },
        { "::ffff:255.255.255.255",                    "::ffff:255.255.255.255" },
        /* "::" standing for exactly one group. */
        { "1:2:3:4:5:6:7::",                           "1:2:3:4:5:6:7:0" },
        { "::2:3:4:5:6:7:8",                           "0:2:3:4:5:6:7:8" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        ip6_addr a;
        memset(&a, 0, sizeof a);
        CHECK(ip6_parse(t[i].in, &a), "parse rejected %s", t[i].in);
        char out[64];
        ip6_format(&a, out, sizeof out);
        CHECK(strcmp(out, t[i].out) == 0,
              "format %s -> %s, want %s", t[i].in, out, t[i].out);
        /* And the canonical form must itself re-parse to the same bytes. */
        ip6_addr b;
        memset(&b, 0, sizeof b);
        CHECK(ip6_parse(out, &b) && ip6_equal(&a, &b),
              "round trip broke for %s (%s)", t[i].in, out);
    }
}

static void test_parse_rejects(void)
{
    /* Every one of these has to be REJECTED, not guessed at. A permissive
     * address parser is a security bug: two readings of one string is how a
     * "block this host" check and the socket that connects disagree. */
    static const char *bad[] = {
        "",  ":",  ":::",  "::::",  "1:",  ":1",
        "1:2:3:4:5:6:7:8:9",            /* too many groups */
        "1:2:3:4:5:6:7",                /* too few, no "::" */
        "1:2:3:4:5:6:7:8:",             /* trailing colon */
        "12345::",                      /* group too long */
        "1::2::3",                      /* two "::" */
        "1:2:3:4:5:6:7::8",             /* "::" standing for zero groups */
        "g::1",  "1::g",  "::-1",
        "fe80::1%eth0",                 /* scope id: one interface, so no */
        "::ffff:999.1.1.1",             /* octet out of range */
        "::ffff:1.2.3",                 /* short quad */
        "::ffff:1.2.3.4.5",             /* long quad */
        "::ffff:01.2.3.4",              /* leading zero = ambiguous octal */
        "1.2.3.4",                      /* not an IPv6 literal at all */
        "2001:db8::1 ",                 /* trailing space */
        " ::1",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ip6_addr a;
        CHECK(!ip6_parse(bad[i], &a), "parser accepted \"%s\"", bad[i]);
    }
    /* A buffer too small must not be written past: report 0, emit nothing. */
    ip6_addr big = A("2001:db8:1234:5678:9abc:def0:1234:5678");
    char tiny[8];
    CHECK(ip6_format(&big, tiny, (int)sizeof tiny) == 0, "short buffer not refused");
    CHECK(tiny[0] == 0,
          "a buffer too small must yield an empty string, never a truncated address");
}

/* ---- classification ------------------------------------------------------ */

static void test_classify(void)
{
    ip6_addr un = A("::"), lo = A("::1"), ll = A("fe80::1"), g = A("2001:db8::1");
    ip6_addr mc = A("ff02::1"), ula = A("fd00::1"), sl = A("fec0::2");
    ip6_addr v4 = A("::ffff:10.0.2.15"), v4ll = A("::ffff:169.254.1.1");

    CHECK(ip6_is_unspecified(&un) && !ip6_is_unspecified(&lo), "unspecified");
    CHECK(ip6_is_loopback(&lo) && !ip6_is_loopback(&un), "loopback");
    CHECK(ip6_is_multicast(&mc) && !ip6_is_multicast(&g), "multicast");
    CHECK(ip6_is_linklocal(&ll) && !ip6_is_linklocal(&g), "link-local");
    /* febf::1 is still fe80::/10; fec0::1 is not. */
    { ip6_addr x = A("febf::1"); CHECK(ip6_is_linklocal(&x), "febf is fe80::/10"); }
    CHECK(!ip6_is_linklocal(&sl), "fec0 is not link-local");
    CHECK(ip6_is_v4mapped(&v4) && !ip6_is_v4mapped(&g), "v4-mapped");

    CHECK(ip6_scope(&ll) == IP6_SCOPE_LINK, "link-local scope");
    CHECK(ip6_scope(&lo) == IP6_SCOPE_LINK, "loopback scope is link (RFC 6724 3.1)");
    CHECK(ip6_scope(&g) == IP6_SCOPE_GLOBAL, "global scope");
    CHECK(ip6_scope(&ula) == IP6_SCOPE_GLOBAL, "ULA scope is global, not site");
    CHECK(ip6_scope(&sl) == IP6_SCOPE_SITE, "fec0::/10 is site scope");
    CHECK(ip6_scope(&mc) == IP6_SCOPE_LINK, "ff02:: is link-scoped multicast");
    { ip6_addr x = A("ff0e::1"); CHECK(ip6_scope(&x) == IP6_SCOPE_GLOBAL,
                                       "ff0e:: is global multicast"); }
    CHECK(ip6_scope(&v4) == IP6_SCOPE_GLOBAL, "private IPv4 is global (RFC 6724 3.2)");
    CHECK(ip6_scope(&v4ll) == IP6_SCOPE_LINK, "169.254/16 is link-local");

    CHECK(ip6_to_v4(&v4) == 0x0A00020Fu, "v4 extraction");
    { ip6_addr r; ip6_from_v4(0x0A00020Fu, &r);
      CHECK(ip6_equal(&r, &v4), "v4 -> mapped round trip"); }

    /* Prefix arithmetic, including the non-byte-aligned case. */
    ip6_addr p = A("2001:db8:1::"), q = A("2001:db8:1:ffff::");
    CHECK(ip6_prefix_eq(&p, &q, 48) && !ip6_prefix_eq(&p, &q, 64), "prefix_eq 48/64");
    CHECK(ip6_common_prefix_len(&p, &q) == 48, "common prefix 48, got %d",
          ip6_common_prefix_len(&p, &q));
    { ip6_addr a1 = A("fc00::"), a2 = A("fe00::");
      CHECK(ip6_prefix_eq(&a1, &a2, 7) == 0, "fc00 vs fe00 differ within /7"); }
    { ip6_addr a1 = A("fc00::"), a2 = A("fd00::");
      CHECK(ip6_prefix_eq(&a1, &a2, 7) == 1, "fc00 and fd00 share /7"); }
    CHECK(ip6_common_prefix_len(&g, &g) == 128, "identical -> 128");

    /* Derived addresses. */
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    ip6_addr lla;
    ip6_linklocal_from_mac(mac, &lla);
    CHECK(strcmp(F(&lla), "fe80::5054:ff:fe12:3456") == 0,
          "EUI-64 link-local = %s", F(&lla));
    ip6_addr sn;
    ip6_solicited_node(&lla, &sn);
    CHECK(strcmp(F(&sn), "ff02::1:ff12:3456") == 0, "solicited-node = %s", F(&sn));
    uint8_t mmac[6];
    ip6_multicast_mac(&sn, mmac);
    CHECK(mmac[0] == 0x33 && mmac[1] == 0x33 && mmac[2] == 0xff &&
          mmac[3] == 0x12 && mmac[4] == 0x34 && mmac[5] == 0x56,
          "solicited-node MAC 33:33:ff:12:34:56");
    ip6_addr an = A("ff02::1");
    ip6_multicast_mac(&an, mmac);
    CHECK(mmac[0] == 0x33 && mmac[1] == 0x33 && mmac[2] == 0 && mmac[5] == 1,
          "all-nodes MAC 33:33:00:00:00:01");
}

/* ---- RFC 6724 policy ----------------------------------------------------- */

static void test_policy(void)
{
    static const struct { const char *a; int prec, label; } t[] = {
        { "::1",                50,  0 },
        { "2001:db8::1",        40,  1 },   /* falls through to ::/0 */
        { "2606:2800::1",       40,  1 },
        { "::ffff:1.2.3.4",     35,  4 },
        { "2002::1",            30,  2 },
        { "2001::1",             5,  5 },   /* Teredo */
        { "fd00::1",             3, 13 },   /* ULA */
        { "fec0::2",             1, 11 },   /* site-local: BELOW IPv4 */
        { "3ffe::1",             1, 12 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        ip6_addr a = A(t[i].a);
        CHECK(ip6_policy_precedence(&a) == t[i].prec,
              "precedence(%s) = %d, want %d", t[i].a,
              ip6_policy_precedence(&a), t[i].prec);
        CHECK(ip6_policy_label(&a) == t[i].label,
              "label(%s) = %d, want %d", t[i].a,
              ip6_policy_label(&a), t[i].label);
    }
    /* The comparison the whole dual-stack question turns on. */
    ip6_addr g = A("2606:2800::1"), v4 = A("::ffff:93.184.216.34"), s = A("fec0::2");
    CHECK(ip6_policy_precedence(&g) > ip6_policy_precedence(&v4),
          "a global v6 destination outranks IPv4");
    CHECK(ip6_policy_precedence(&s) < ip6_policy_precedence(&v4),
          "SLIRP's fec0::/10 ranks BELOW IPv4 -- prefer-v6 is not a constant");
}

/* ---- RFC 6724 s5: source selection --------------------------------------- */

static struct ip6_src_cand mk(const char *a, int plen, int state)
{
    struct ip6_src_cand c;
    memset(&c, 0, sizeof c);
    c.addr = A(a);
    c.plen = (uint8_t)plen;
    c.state = (uint8_t)state;
    return c;
}

static void test_source_selection(void)
{
    struct ip6_src_cand c[6];
    int n;

    /* A typical dual-stack host. */
    c[0] = mk("fe80::5054:ff:fe12:3456", 64, IP6_PREFERRED);
    c[1] = mk("2001:db8:1::5054:ff:fe12:3456", 64, IP6_PREFERRED);
    c[2] = mk("2001:db8:2::1", 64, IP6_DEPRECATED);
    c[3] = mk("::ffff:10.0.2.15", 96, IP6_PREFERRED);
    n = 4;

    { ip6_addr d = A("fe80::2");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 0, "rule 2: a link-local destination sources from link-local (got %d)", i); }

    { ip6_addr d = A("2001:db8:1::99");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 1, "rule 8: longest matching prefix (got %d)", i); }

    { ip6_addr d = A("2606:2800::1");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 1, "rule 3: the preferred global beats the deprecated one (got %d)", i); }

    { ip6_addr d = A("::ffff:8.8.8.8");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 3, "a v4-mapped destination only ever sources from IPv4 (got %d)", i); }

    { ip6_addr d = A("2001:db8:1::5054:ff:fe12:3456");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 1, "rule 1: prefer the destination's own address (got %d)", i); }

    /* Rule 3 in isolation: the ONLY global candidate is deprecated, so it is
     * still selected -- rule 3 orders candidates, it does not veto them. */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("2001:db8:2::1", 64, IP6_DEPRECATED);
    n = 2;
    { ip6_addr d = A("2606:2800::1");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 1, "a deprecated address is a last resort, not forbidden (got %d)", i); }

    /* DAD is enforced HERE, which is the point: a tentative address must never
     * source a packet, even when it is the only candidate that fits. */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("2001:db8:2::1", 64, IP6_TENTATIVE);
    n = 2;
    { ip6_addr d = A("2606:2800::1");
      int i = ip6_select_source(&d, c, n);
      CHECK(i < 0, "a tentative address must not be selected (got %d)", i); }

    /* No candidate at all. */
    { ip6_addr d = A("2606:2800::1");
      CHECK(ip6_select_source(&d, c, 0) < 0, "empty candidate set -> -1"); }

    /* Site-local prefers the site-local source by label AND by scope. */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("fec0::5054:ff:fe12:3456", 64, IP6_PREFERRED);
    c[2] = mk("2001:db8:1::1", 64, IP6_PREFERRED);
    n = 3;
    { ip6_addr d = A("fec0::2");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 1, "rule 2/6: fec0 destination sources from fec0 (got %d)", i); }

    /* Rule 2 the other way: a link-local source is too small for a global
     * destination, so the larger scope wins even though it is "further". */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("2001:db8:1::1", 64, IP6_PREFERRED);
    n = 2;
    { ip6_addr d = A("2606:2800::1");
      int i = ip6_select_source(&d, c, n);
      CHECK(i == 1, "rule 2: link-local is too small for a global destination (got %d)", i); }
}

/* ---- RFC 6724 s6: destination ordering ----------------------------------- */

static void order(ip6_addr *d, int n, struct ip6_src_cand *c, int nc)
{
    ip6_sort_dests(d, n, c, nc);
}

static void test_dest_ordering(void)
{
    struct ip6_src_cand c[4];
    ip6_addr d[4];

    /* Dual stack, working v6: the global v6 destination goes first (rule 6,
     * precedence 40 vs 35). */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("2001:db8:1::1", 64, IP6_PREFERRED);
    c[2] = mk("::ffff:10.0.2.15", 96, IP6_PREFERRED);
    d[0] = A("::ffff:93.184.216.34");
    d[1] = A("2606:2800:220:1:248:1893:25c8:1946");
    order(d, 2, c, 3);
    CHECK(!ip6_is_v4mapped(&d[0]), "working v6: IPv6 destination first (got %s)", F(&d[0]));
    CHECK(ip6_is_v4mapped(&d[1]), "working v6: IPv4 second");

    /* THE case that matters most. No global v6 source -- a link-local-only
     * host, which is what a network with RAs but no working path upstream looks
     * like. Rule 1 (avoid unusable destinations) must push the v6 address
     * BELOW IPv4, or this OS becomes unable to reach dual-stack hosts. */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("::ffff:10.0.2.15", 96, IP6_PREFERRED);
    d[0] = A("2606:2800:220:1:248:1893:25c8:1946");
    d[1] = A("::ffff:93.184.216.34");
    order(d, 2, c, 2);
    CHECK(ip6_is_v4mapped(&d[0]),
          "no v6 source: IPv4 must come first (got %s)", F(&d[0]));

    /* QEMU SLIRP's default prefix. We DO have a usable fec0:: source, yet the
     * policy table ranks fec0::/10 at precedence 1 against IPv4's 35, so the
     * correct answer is still IPv4 first. */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("fec0::5054:ff:fe12:3456", 64, IP6_PREFERRED);
    c[2] = mk("::ffff:10.0.2.15", 96, IP6_PREFERRED);
    d[0] = A("fec0::2");
    d[1] = A("::ffff:10.0.2.2");
    order(d, 2, c, 3);
    CHECK(ip6_is_v4mapped(&d[0]),
          "fec0::/10 ranks below IPv4 (got %s first)", F(&d[0]));

    /* Rule 3: between two v6 destinations, the one whose source is preferred
     * beats the one whose source is deprecated. */
    c[0] = mk("2001:db8:1::1", 64, IP6_DEPRECATED);
    c[1] = mk("2001:db8:2::1", 64, IP6_PREFERRED);
    d[0] = A("2001:db8:1::9");     /* sourced from the deprecated address */
    d[1] = A("2001:db8:2::9");     /* sourced from the preferred one */
    order(d, 2, c, 2);
    CHECK(EQ(&d[0], "2001:db8:2::9"),
          "rule 3: preferred source wins (got %s)", F(&d[0]));

    /* Rule 8: a smaller scope is preferred between otherwise-equal
     * destinations. Link-local before global, given sources for both. */
    c[0] = mk("fe80::1", 64, IP6_PREFERRED);
    c[1] = mk("2001:db8:1::1", 64, IP6_PREFERRED);
    d[0] = A("2001:db8:9::1");
    d[1] = A("fe80::2");
    order(d, 2, c, 2);
    CHECK(ip6_is_linklocal(&d[0]), "rule 8: smaller scope first (got %s)", F(&d[0]));

    /* Rule 10 (stability): destinations the rules cannot separate keep the
     * order the resolver gave them, which is how round-robin DNS stays
     * round-robin instead of collapsing onto one server. */
    c[0] = mk("2001:db8:1::1", 64, IP6_PREFERRED);
    d[0] = A("2001:db8:9::1");
    d[1] = A("2001:db8:9::2");
    d[2] = A("2001:db8:9::3");
    order(d, 3, c, 1);
    CHECK(EQ(&d[0], "2001:db8:9::1") &&
          EQ(&d[1], "2001:db8:9::2") &&
          EQ(&d[2], "2001:db8:9::3"),
          "rule 10: equal destinations keep their order");

    /* Rule 9: longest matching prefix separates same-family destinations. */
    c[0] = mk("2001:db8:1::1", 64, IP6_PREFERRED);
    d[0] = A("2001:dba::1");
    d[1] = A("2001:db8:1::99");
    order(d, 2, c, 1);
    CHECK(EQ(&d[0], "2001:db8:1::99"),
          "rule 9: longest matching prefix first (got %s)", F(&d[0]));

    /* A pure v4-only host must be completely unaffected: one family in, same
     * order out. This is the no-regression property stated as a unit test. */
    c[0] = mk("::ffff:10.0.2.15", 96, IP6_PREFERRED);
    d[0] = A("::ffff:1.1.1.1");
    d[1] = A("::ffff:8.8.8.8");
    d[2] = A("::ffff:9.9.9.9");
    order(d, 3, c, 1);
    CHECK(EQ(&d[0], "::ffff:1.1.1.1") &&
          EQ(&d[1], "::ffff:8.8.8.8") &&
          EQ(&d[2], "::ffff:9.9.9.9"),
          "v4-only ordering is identity");
}

int main(void)
{
    test_parse_format();
    test_parse_rejects();
    test_classify();
    test_policy();
    test_source_selection();
    test_dest_ordering();
    printf("ip6_addr: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
