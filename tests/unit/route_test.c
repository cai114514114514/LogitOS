/* The IPv4 forwarding table, on the host.
 *
 * White-box in the shape tests/unit/arp_test.c and tests/unit/ip_arp_test.c
 * established: it #includes the implementation. Unlike those two it stubs
 * NOTHING, because there is nothing below c/net/core/route.c to stub -- no
 * device, no clock, no console, no allocator. That is the property the file
 * was written for and this test is the demonstration of it: a routing decision
 * is arithmetic over integers, so it can be checked exhaustively without a
 * kernel, and the thing it is checked against is a table of expected answers
 * rather than a second implementation of the same rules.
 *
 * The gate is table-driven: `routes[]` is a configuration and `cases[]` is a
 * set of destinations with the (interface, next hop, prefix) each must resolve
 * to. Adding a case is a line.
 *
 * NEGATIVE CONTROL: -DROUTE_FIRST_MATCH removes the RANKING from
 * route_lookup() and nothing else -- every route still matches correctly and
 * every field is still carried out, so the answer is a well-formed interface
 * and next hop that happens to be the wrong one. See the count at the bottom
 * of tests/route.mk for exactly which checks that reddens and why.
 */

#include <stdio.h>
#include <stdint.h>

#include "route.c"

/* ---- harness ------------------------------------------------------------ */

static int checks, failures;

#define IPV4T(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)| \
                        ((uint32_t)(c)<<8)|(uint32_t)(d))

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) { printf("ok   %s\n", what); return; }
    failures++;
    printf("FAIL %s\n", what);
}

/* Four rotating buffers so two addresses can be printed in one printf. */
static const char *ips(uint32_t a)
{
    static char b[4][20];
    static int n;
    char *p = b[n = (n + 1) & 3];
    snprintf(p, sizeof b[0], "%u.%u.%u.%u",
             (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return p;
}

/* ---- the configuration under test --------------------------------------- */

#define LO    RT_OIF_LO      /* 1 */
#define ETH0  RT_OIF_NIC0    /* 2 */
#define ETH1  3

#define GW      IPV4T(10,0,2,2)
#define VIA_A   IPV4T(10,0,2,9)
#define VIA_B   IPV4T(10,0,2,10)
#define VIA_C   IPV4T(10,0,2,11)
#define MY_IP   IPV4T(10,0,2,15)
#define ETH1_IP IPV4T(172,20,0,5)

/* INSERTION ORDER IS PART OF THE TEST. The default route is installed FIRST,
 * which is what a real configuration produces -- DHCP hands over an address
 * and a gateway in the same message -- and it is what makes the negative
 * control bite: a table whose first row matches every destination in the
 * universe answers every lookup with the gateway, which is precisely what
 * ip.c's old ternary did. */
static const struct route_entry routes[] = {
    /* the default route: prefix length 0 and nothing else. Not a field, not a
     * special case, not a fallback branch. */
    { .dst = 0,                   .plen = 0,  .nexthop = GW,    .oif = ETH0,
      .src = MY_IP, .metric = 0 },
    /* connected: the subnet the primary card is on */
    { .dst = IPV4T(10,0,2,0),     .plen = 24, .nexthop = 0,     .oif = ETH0,
      .src = MY_IP, .metric = 0 },
    /* loopback, as an interface and not as a branch in ip.c */
    { .dst = IPV4T(127,0,0,0),    .plen = 8,  .nexthop = 0,     .oif = LO,
      .src = IPV4T(127,0,0,1), .metric = 0, .flags = RT_F_LOCAL },
    /* two routes to the SAME prefix, the worse one installed first */
    { .dst = IPV4T(192,168,7,0),  .plen = 24, .nexthop = VIA_A, .oif = ETH0,
      .src = MY_IP, .metric = 100 },
    { .dst = IPV4T(192,168,7,0),  .plen = 24, .nexthop = VIA_B, .oif = ETH0,
      .src = MY_IP, .metric = 10 },
    /* a host route inside that prefix, out of a DIFFERENT interface */
    { .dst = IPV4T(192,168,7,42), .plen = 32, .nexthop = VIA_C, .oif = ETH1,
      .src = ETH1_IP, .metric = 0 },
};
#define NROUTES ((int)(sizeof routes / sizeof routes[0]))

/* `decided_by` records WHY a case has the answer it has, so the negative
 * control's damage can be predicted and counted instead of eyeballed. */
enum { BY_ONLY_MATCH, BY_PREFIX, BY_METRIC };

static const struct {
    const char *what;
    uint32_t dst;
    int      want_rc;
    int      want_oif;
    uint32_t want_nexthop;
    uint32_t want_src;
    int      want_plen;
    int      decided_by;
} cases[] = {
  { "on-link: a host on our own subnet is its own next hop",
    IPV4T(10,0,2,9),     RT_OK, ETH0, IPV4T(10,0,2,9),  MY_IP, 24, BY_PREFIX },
  { "on-link: the gateway itself is reached directly, not via itself",
    GW,                  RT_OK, ETH0, GW,               MY_IP, 24, BY_PREFIX },
  { "default: an internet address takes the /0 via the gateway",
    IPV4T(8,8,8,8),      RT_OK, ETH0, GW,               MY_IP,  0, BY_ONLY_MATCH },
  { "default: a private address we have no route to also takes the /0",
    IPV4T(172,16,0,1),   RT_OK, ETH0, GW,               MY_IP,  0, BY_ONLY_MATCH },
  { "loopback: 127.0.0.1 goes to lo, NOT to the gateway",
    IPV4T(127,0,0,1),    RT_OK, LO,   IPV4T(127,0,0,1),
                                          IPV4T(127,0,0,1),  8, BY_PREFIX },
  { "loopback: the whole 127/8 block, not just .1",
    IPV4T(127,0,0,2),    RT_OK, LO,   IPV4T(127,0,0,2),
                                          IPV4T(127,0,0,1),  8, BY_PREFIX },
  { "loopback: 127.255.255.254 is still loopback",
    IPV4T(127,255,255,254), RT_OK, LO, IPV4T(127,255,255,254),
                                          IPV4T(127,0,0,1),  8, BY_PREFIX },
  { "metric: the cheaper of two routes to the same prefix wins",
    IPV4T(192,168,7,5),  RT_OK, ETH0, VIA_B,            MY_IP, 24, BY_METRIC },
  { "longest prefix: a /32 host route beats the /24 it sits inside",
    IPV4T(192,168,7,42), RT_OK, ETH1, VIA_C,            ETH1_IP, 32, BY_PREFIX },
  { "longest prefix: the /32's neighbour still takes the /24",
    IPV4T(192,168,7,43), RT_OK, ETH0, VIA_B,            MY_IP, 24, BY_METRIC },
};
#define NCASES ((int)(sizeof cases / sizeof cases[0]))

static void install_routes(void)
{
    route_flush();
    for (int i = 0; i < NROUTES; i++) {
        int rc = route_add(routes[i]);
        if (rc != RT_OK) {
            printf("FAIL route_add(%s/%u) returned %d\n",
                   ips(routes[i].dst), routes[i].plen, rc);
            failures++;
        }
        checks++;
    }
    printf("ok   installed %d routes, table holds %d\n", NROUTES, route_count());
}

/* ---- the table-driven pass ---------------------------------------------- */

static void run_cases(void)
{
    char msg[200];
    for (int i = 0; i < NCASES; i++) {
        struct route_res r;
        /* Poison the result so a lookup that returns without filling it in
         * cannot be mistaken for one that agreed with us by luck. */
        r.oif = -999; r.nexthop = 0xDEADBEEF; r.src = 0xDEADBEEF;
        r.plen = 255; r.flags = 0xFFFFFFFFu; r.metric = -999;

        int rc = route_lookup(cases[i].dst, &r);
        int good = (rc == cases[i].want_rc);
        if (good && rc == RT_OK)
            good = r.oif == cases[i].want_oif &&
                   r.nexthop == cases[i].want_nexthop &&
                   r.src == cases[i].want_src &&
                   r.plen == cases[i].want_plen;

        snprintf(msg, sizeof msg, "%s  [%s -> dev %d via %s /%u]",
                 cases[i].what, ips(cases[i].dst), r.oif, ips(r.nexthop), r.plen);
        ok(good, msg);
        if (!good)
            printf("       wanted dev %d via %s /%d (rc %d, got %d)\n",
                   cases[i].want_oif, ips(cases[i].want_nexthop),
                   cases[i].want_plen, cases[i].want_rc, rc);
    }

    /* The loopback route is the one that carries a flag, and the flag is what
     * ip_send reads to know not to look for a neighbour. */
    struct route_res r;
    route_lookup(IPV4T(127,0,0,1), &r);
    ok((r.flags & RT_F_LOCAL) != 0, "loopback route carries RT_F_LOCAL");
    route_lookup(IPV4T(8,8,8,8), &r);
    ok((r.flags & RT_F_LOCAL) == 0, "the default route does not carry RT_F_LOCAL");
}

/* ---- no route at all ---------------------------------------------------- */

static void no_route(void)
{
    /* THE CASE THE OLD CODE COULD NOT EXPRESS. Withdraw the default and ask
     * for something outside every remaining prefix: the answer must be a
     * refusal, and it must not be the gateway. */
    route_flush();
    checks++;
    if (route_add(routes[1]) != RT_OK) { failures++; printf("FAIL setup\n"); }
    else printf("ok   setup: connected route only, no default\n");

    struct route_res r;
    r.oif = -999; r.nexthop = 0xDEADBEEF;
    int rc = route_lookup(IPV4T(8,8,8,8), &r);
    ok(rc == RT_ENOROUTE, "no route: an unroutable destination is refused");
    ok(rc != RT_OK, "no route: the refusal is not a successful lookup");
    ok(r.nexthop != GW,
       "no route: the refusal is NOT the gateway (the old ternary's answer)");
    ok(r.oif == -999, "no route: the result struct is left untouched");

    /* ...and the destination that IS covered still resolves, so the refusal is
     * discriminating and not a table that stopped working. */
    rc = route_lookup(IPV4T(10,0,2,7), &r);
    ok(rc == RT_OK && r.oif == ETH0 && r.nexthop == IPV4T(10,0,2,7),
       "no route: the covered destination still resolves on-link");

    /* Deleting a route that is not there is a refusal too, not a silent 0. */
    ok(route_del(IPV4T(1,2,3,0), 24, 0) == RT_ENOROUTE,
       "route_del of an absent route reports RT_ENOROUTE");
}

/* ---- metric independent of insertion order ------------------------------ */

static void metric_order(void)
{
    /* The metric case in `cases[]` installs the worse route first. Here it is
     * installed the other way round, and the answer must not move: a tie-break
     * that only works in one insertion order is a coincidence, not a rule. */
    route_flush();
    route_add((struct route_entry){ .dst = IPV4T(192,168,7,0), .plen = 24,
                                    .nexthop = VIA_B, .oif = ETH0, .metric = 10 });
    route_add((struct route_entry){ .dst = IPV4T(192,168,7,0), .plen = 24,
                                    .nexthop = VIA_A, .oif = ETH0, .metric = 100 });
    struct route_res r;
    route_lookup(IPV4T(192,168,7,5), &r);
    ok(r.nexthop == VIA_B && r.metric == 10,
       "metric: cheaper route wins when it is installed FIRST too");

    /* Equal metric, equal prefix: the earlier one wins, deterministically.
     * Not an arbitrary choice -- an answer that depends on slot reuse would
     * make every other assertion in this file order-dependent. */
    route_flush();
    route_add((struct route_entry){ .dst = IPV4T(203,0,113,0), .plen = 24,
                                    .nexthop = VIA_A, .oif = ETH0, .metric = 5 });
    route_add((struct route_entry){ .dst = IPV4T(203,0,113,0), .plen = 24,
                                    .nexthop = VIA_B, .oif = ETH1, .metric = 5 });
    route_lookup(IPV4T(203,0,113,9), &r);
    ok(r.nexthop == VIA_A && r.oif == ETH0,
       "equal prefix and metric: the earlier route wins, stably");
}

/* ---- masks and prefix arithmetic ---------------------------------------- */

static void masks(void)
{
    ok(route_plen_mask(0)  == 0x00000000u, "plen 0 mask is 0 (no 32-bit shift UB)");
    ok(route_plen_mask(1)  == 0x80000000u, "plen 1 mask");
    ok(route_plen_mask(24) == 0xFFFFFF00u, "plen 24 mask");
    ok(route_plen_mask(32) == 0xFFFFFFFFu, "plen 32 mask");

    ok(route_mask_plen(0x00000000u) == 0,  "mask 0.0.0.0 is /0");
    ok(route_mask_plen(0xFFFFFF00u) == 24, "mask 255.255.255.0 is /24");
    ok(route_mask_plen(0xFFFFFFFFu) == 32, "mask 255.255.255.255 is /32");
    ok(route_mask_plen(0xFF000000u) == 8,  "mask 255.0.0.0 is /8");
    /* Refused, not guessed at: 255.255.0.255 is somebody's mistake and both
     * readings of it are wrong. A popcount would call this a /24. */
    ok(route_mask_plen(0xFFFF00FFu) == -1, "a mask with a hole is refused, not rounded");
    ok(route_mask_plen(0x0000FFFFu) == -1, "an inverted mask is refused");
}

/* ---- insertion rules ---------------------------------------------------- */

static void insertion(void)
{
    route_flush();
    ok(route_add((struct route_entry){ .dst = 0, .plen = 33, .oif = ETH0 })
       == RT_EINVAL, "route_add refuses a prefix length above 32");
    ok(route_add((struct route_entry){ .dst = 0, .plen = 24, .oif = 0 })
       == RT_EINVAL, "route_add refuses interface index 0");
    ok(route_add((struct route_entry){ .dst = 0, .plen = 24, .oif = -1 })
       == RT_EINVAL, "route_add refuses a negative interface index");
    ok(route_count() == 0, "no refused route reached the table");

    /* An unnormalised prefix is stored normalised, so a later compare against
     * the canonical form matches. */
    route_add((struct route_entry){ .dst = IPV4T(10,1,2,15), .plen = 24,
                                    .oif = ETH0, .nexthop = GW });
    const struct route_entry *e = NULL;
    for (int i = 0; i < RT_NROUTE; i++) if ((e = route_at(i))) break;
    ok(e && e->dst == IPV4T(10,1,2,0), "route_add masks dst to the prefix length");

    /* Restating the same route updates it in place rather than consuming a
     * second slot -- which is what makes route_v4_iface idempotent. */
    int before = route_count();
    route_add((struct route_entry){ .dst = IPV4T(10,1,2,0), .plen = 24,
                                    .oif = ETH0, .nexthop = GW, .metric = 7 });
    ok(route_count() == before, "restating a route does not consume a slot");
    for (int i = 0; i < RT_NROUTE; i++) if ((e = route_at(i))) break;
    ok(e && e->metric == 7, "restating a route updates its metric in place");

    /* A full table refuses. It does NOT evict: a route dropped to make room
     * for another is a machine that becomes unreachable with nothing logged. */
    route_flush();
    int added = 0;
    for (int i = 0; i < RT_NROUTE + 4; i++) {
        struct route_entry r = { .dst = IPV4T(10,(uint8_t)i,0,0), .plen = 16,
                                 .oif = ETH0, .nexthop = GW };
        if (route_add(r) == RT_OK) added++;
    }
    ok(added == RT_NROUTE, "the table accepts exactly RT_NROUTE routes");
    ok(route_add((struct route_entry){ .dst = IPV4T(11,0,0,0), .plen = 8,
                                       .oif = ETH0, .nexthop = GW }) == RT_EFULL,
       "a full table refuses with RT_EFULL rather than evicting");
    ok(route_count() == RT_NROUTE, "nothing was evicted to make room");
}

/* ---- the address -> routes bridge --------------------------------------- */

static void iface_config(void)
{
    route_flush();
    uint32_t g0 = route_generation();
    ok(route_v4_iface(ETH0, MY_IP, 0xFFFFFF00u, GW, 0) == RT_OK,
       "route_v4_iface accepts 10.0.2.15/24 gw 10.0.2.2");
    ok(route_count() == 2,
       "an address plus a gateway implies exactly two routes");
    ok(route_generation() != g0, "the generation moved");

    struct route_res r;
    ok(route_lookup(IPV4T(10,0,2,9), &r) == RT_OK && r.plen == 24 &&
       r.nexthop == IPV4T(10,0,2,9) && r.src == MY_IP,
       "the implied connected route resolves on-link with the right source");
    ok(route_lookup(IPV4T(8,8,8,8), &r) == RT_OK && r.plen == 0 &&
       r.nexthop == GW, "the implied default route resolves via the gateway");

    /* IDEMPOTENCE is what lets ip_send call this on every datagram. */
    uint32_t g1 = route_generation();
    route_v4_iface(ETH0, MY_IP, 0xFFFFFF00u, GW, 0);
    route_v4_iface(ETH0, MY_IP, 0xFFFFFF00u, GW, 0);
    ok(route_generation() == g1 && route_count() == 2,
       "restating the same configuration changes nothing at all");

    /* A NEW LEASE MUST NOT LEAVE THE OLD SUBNET BEHIND. The stale connected
     * route would keep winning on prefix length while pointing at a subnet the
     * card no longer has an address in. */
    route_v4_iface(ETH0, IPV4T(192,168,1,50), 0xFFFFFF00u, IPV4T(192,168,1,1), 0);
    ok(route_count() == 2, "a new lease replaces the interface's routes");
    ok(route_lookup(IPV4T(10,0,2,9), &r) == RT_ENOROUTE ||
       (route_lookup(IPV4T(10,0,2,9), &r) == RT_OK && r.plen == 0),
       "the previous subnet is no longer on-link");
    ok(route_lookup(IPV4T(192,168,1,9), &r) == RT_OK && r.plen == 24,
       "the new subnet is on-link");

    /* No gateway: one route, and no fabricated default. A default route
     * through nowhere is exactly the failure this whole file exists to end. */
    route_flush();
    route_v4_iface(ETH1, IPV4T(172,20,0,5), 0xFFFF0000u, 0, 0);
    ok(route_count() == 1, "an address with no gateway implies ONE route");
    ok(route_lookup(IPV4T(8,8,8,8), &r) == RT_ENOROUTE,
       "no gateway means no default route, not a default via 0.0.0.0");

    /* Loopback goes through the same bridge as every other interface -- that
     * is the point of it being an interface. */
    route_v4_iface(LO, IPV4T(127,0,0,1), 0xFF000000u, 0, RT_F_LOCAL);
    ok(route_lookup(IPV4T(127,0,0,1), &r) == RT_OK && r.oif == LO &&
       (r.flags & RT_F_LOCAL), "loopback is configured by the same call");

    /* Refusals. */
    ok(route_v4_iface(ETH0, MY_IP, 0xFFFF00FFu, GW, 0) == RT_EINVAL,
       "route_v4_iface refuses a netmask with a hole");
    ok(route_v4_iface(0, MY_IP, 0xFFFFFF00u, GW, 0) == RT_EINVAL,
       "route_v4_iface refuses interface index 0");
    ok(route_v4_iface(RT_NIF, MY_IP, 0xFFFFFF00u, GW, 0) == RT_EINVAL,
       "route_v4_iface refuses an index it cannot memoise");

    /* route_flush_if takes the memo with it, or the next configure would be a
     * no-op against an empty table. */
    route_flush();
    route_v4_iface(ETH0, MY_IP, 0xFFFFFF00u, GW, 0);
    route_flush_if(ETH0);
    ok(route_count() == 0, "route_flush_if withdrew the interface's routes");
    route_v4_iface(ETH0, MY_IP, 0xFFFFFF00u, GW, 0);
    ok(route_count() == 2, "and the memo went with them, so a reconfigure works");
}

int main(void)
{
#ifdef ROUTE_FIRST_MATCH
    printf("route_test: NEGATIVE CONTROL build (ROUTE_FIRST_MATCH)\n");
#endif
    install_routes();
    run_cases();
    no_route();
    metric_order();
    masks();
    insertion();
    iface_config();

    /* How many of the table-driven cases the control is EXPECTED to break,
     * printed by both builds so the two numbers can be compared directly
     * instead of remembered. */
    int by_prefix = 0, by_metric = 0;
    for (int i = 0; i < NCASES; i++) {
        if (cases[i].decided_by == BY_PREFIX) by_prefix++;
        if (cases[i].decided_by == BY_METRIC) by_metric++;
    }
    printf("\nroute_test: %d checks, %d failed  "
           "(%d cases decided by prefix length, %d by metric)\n",
           checks, failures, by_prefix, by_metric);
    if (failures) { printf("route_test: FAILURES\n"); return 1; }
    printf("route_test: ALL PASS\n");
    return 0;
}
