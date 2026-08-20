/* Host-side IPv4/UDP/ICMP/DNS protocol tests. This is a white-box translation
 * unit: it includes the implementation files and stubs only ARP, Ethernet,
 * the RNG, and the timer, so checksums, fragment reassembly, and IP dispatch
 * run exactly as they do in the kernel. Built with -DLOGIT_NET_HOST so
 * net_lock() degenerates to a no-op (cli/sti are ring-0 only). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "eth.h"
#include "arp.h"
#include "pit.h"
#include "rng.h"

#define LOCAL_IP 0x0A00020Fu       /* 10.0.2.15 */
#define REMOTE_IP 0x0A000202u      /* 10.0.2.2  */
#define DNS_IP   0x0A000203u       /* 10.0.2.3  */

struct net_config net_cfg = {
    .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .ip = LOCAL_IP, .mask = 0xFFFFFF00u, .gw = REMOTE_IP, .dns = DNS_IP,
};

static uint8_t wire[1600];
static uint16_t wire_len;
static int eth_sends;
static uint8_t eth_last_dst[ETH_ALEN];
static int arp_calls;
static uint64_t ticks;

uint64_t timer_ticks(void) { return ticks; }
int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    (void)ip;
    arp_calls++;
    memset(mac, 0xAB, ETH_ALEN);
    return 0;
}
int arp_warm(uint32_t ip, int timeout) { (void)ip; (void)timeout; return 0; }

/* ip_send() now hands unicast datagrams to arp_output (resolve, or HOLD until
 * resolution completes) instead of dropping them on a miss. This stub keeps
 * the always-resolves behaviour the tests below were written against: it
 * counts the call as a resolution and transmits immediately, so every
 * assertion about IP framing still sees exactly the frame it did before.
 * The queueing behaviour itself is tested in tests/unit/ip_arp_test.c against
 * the REAL arp.c, which is where it belongs. */
int arp_output(uint32_t nexthop, uint16_t ethertype, const void *payload, uint16_t len)
{
    uint8_t mac[ETH_ALEN];
    (void)arp_resolve(nexthop, mac);
    return eth_send(mac, ethertype, payload, len);
}

/* IPv6 reachability, stubbed to "no v6" so the v4 tests below mean what they
 * always meant. dns.c asks these two before it will send a AAAA query
 * (`want6 = (ip6_up() == 2)`), and the whole of ip6.c cannot come into this
 * translation unit to answer them: it drags in nd.c, icmp6, tcp_input_af,
 * net_up and kprintf -- the entire kernel network layer, in a test whose point
 * is to be a small white-box unit. The address helpers this file DOES exercise
 * live in ip6_addr.c, which is self-contained and is linked in.
 *
 * A v6-enabled variant of these paths needs its own target with the real
 * module and its dependencies, not a wider link line here. */
int ip6_up(void) { return 0; }
struct ip6_src_cand;
int ip6_dual_candidates(struct ip6_src_cand *cand, int max)
{ (void)cand; (void)max; return 0; }
int eth_send(const uint8_t dst[ETH_ALEN], uint16_t type,
             const void *payload, uint16_t len)
{
    if (type != ETHERTYPE_IP || len > sizeof wire) return -1;
    memcpy(eth_last_dst, dst, ETH_ALEN);
    memcpy(wire, payload, len);
    wire_len = len;
    eth_sends++;
    return 0;
}
const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
void net_poll(void) {}
void net_idle(void) {}
void kernel_random_bytes(uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) out[i] = (uint8_t)(0x5Au + (unsigned)i);
}

/* dns.c's TCP fallback needs tcp.c's client API to LINK. Nothing below sets
 * TC on a response, so the fallback is never actually taken in this file --
 * see tests/unit/dns_test.c for that path exercised against a model TCP. */
int  tcp_connect_start(uint32_t dst, uint16_t port) { (void)dst; (void)port; return -1; }
int  tcp_connect_status(int id) { (void)id; return -1; }
int  tcp_send_nb(int id, const void *b, int len) { (void)id; (void)b; (void)len; return -1; }
int  tcp_recv(int id, void *b, int max) { (void)id; (void)b; (void)max; return -1; }
void tcp_close(int id) { (void)id; }

/* ip.c grew a routing decision (route_sync()/route_lookup(), 2026-08-20) and
 * this link line did not follow -- test-net-proto stopped LINKING, which is
 * exactly the rot CLAUDE.md's test-suite section names: "a source file grew a
 * dependency and a link line did not follow". Two groups of undefined symbol:
 *
 *   route_*   -- route.c is included WHOLE, the same white-box way ip.c and
 *                udp.c below are, rather than added to the compile line. The
 *                compile line lives in the main Makefile, which several
 *                concurrent workflows are holding; a fix that has to land in
 *                a file somebody else owns is a fix that does not land. It
 *                costs nothing here: `nm -u build/c/net/core/route.o` is
 *                EMPTY -- route.c knows only integers (an interface is an
 *                `int oif`), so it drags in no netdev, no PCI, no driver.
 *                tests/link.mk's ip_arp_test passes it on the command line
 *                instead; both end up as one translation unit either way.
 *
 *   kprintf   -- ip.c prints the table whenever it changes. Swallowed rather
 *                than forwarded to stdout: this file's output is a list of
 *                pass/fail lines a person reads, and a route dump interleaved
 *                through them is noise. ip_arp_test.c stubs it identically.
 *
 * REJECTED: stubbing route_lookup() to return the old on-link-or-gateway
 * ternary. That would make this file test a routing decision the kernel no
 * longer makes -- worse than not testing it, because the gate would stay
 * green straight through a real regression in route.c. */
void kprintf(const char *fmt, ...) { (void)fmt; }
#include "route.c"

/* Avoid fortified memcpy macros conflicting with the kernel prototypes. */
#undef memcpy
#undef memset
#include "ip.c"
#include "reasm.c"
#include "icmp.c"
#include "udp.c"
#include "dns.c"

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* IP identification for frames built from now on; tests that poke reassembly
 * state set a distinct id per scenario so leftover slots cannot collide. */
static uint16_t frame_id;

static int make_frame(uint8_t *frame, uint32_t src, uint32_t dst,
                      uint8_t proto, const void *l4, uint16_t l4len,
                      uint16_t frag, uint8_t ttl)
{
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memset(eh, 0, sizeof *eh);
    eh->ethertype = htons(ETHERTYPE_IP);
    struct ip_hdr *ih = (struct ip_hdr *)(frame + sizeof *eh);
    memset(ih, 0, sizeof *ih);
    ih->ver_ihl = 0x45;
    ih->total_len = htons((uint16_t)(sizeof *ih + l4len));
    ih->id = htons(frame_id);
    ih->frag = htons(frag);
    ih->ttl = ttl;
    ih->proto = proto;
    ih->src = htonl(src); ih->dst = htonl(dst);
    ih->checksum = htons(ip_checksum(ih, sizeof *ih));
    memcpy((uint8_t *)ih + sizeof *ih, l4, l4len);
    return (int)(sizeof *eh + sizeof *ih + l4len);
}

static uint16_t make_udp(uint8_t *out, uint32_t src, uint32_t dst,
                         uint16_t sport, uint16_t dport,
                         const uint8_t *payload, uint16_t plen, int with_checksum)
{
    struct udp_hdr *h = (struct udp_hdr *)out;
    uint16_t len = (uint16_t)(sizeof *h + plen);
    h->sport = htons(sport); h->dport = htons(dport);
    h->length = htons(len); h->checksum = 0;
    memcpy(out + sizeof *h, payload, plen);
    if (with_checksum) {
        uint16_t sum = udp_checksum(src, dst, out, len);
        h->checksum = htons(sum ? sum : 0xFFFFu);
    }
    return len;
}

/* Build an ICMP error message (type/code) quoting an "original" packet: a
 * fresh IP header (osrc -> odst, oproto) plus the first 8 bytes of its L4. */
static uint16_t make_icmp_error(uint8_t *out, int type, int code,
                                uint32_t osrc, uint32_t odst, uint8_t oproto,
                                const uint8_t *ol4, uint16_t ol4len)
{
    struct icmp_hdr *h = (struct icmp_hdr *)out;
    h->type = (uint8_t)type; h->code = (uint8_t)code;
    h->checksum = 0; h->id = 0; h->seq = 0;
    struct ip_hdr *oh = (struct ip_hdr *)(out + sizeof *h);
    memset(oh, 0, sizeof *oh);
    oh->ver_ihl = 0x45;
    oh->total_len = htons((uint16_t)(sizeof *oh + ol4len));
    oh->ttl = 64; oh->proto = oproto;
    oh->src = htonl(osrc); oh->dst = htonl(odst);
    oh->checksum = htons(ip_checksum(oh, sizeof *oh));
    memcpy(out + sizeof *h + sizeof *oh, ol4, 8);
    uint16_t len = (uint16_t)(sizeof *h + sizeof *oh + 8);
    h->checksum = htons(ip_checksum(out, len));
    return len;
}

/* White-box helper: reasm.c's slots are visible in this translation unit. */
static int used_slots(void)
{
    int n = 0;
    for (int i = 0; i < REASM_SLOTS; i++) n += slots[i].used;
    return n;
}

/* White-box reset of the UDP socket table between groups (dns.c's legacy
 * query slot is invalidated along with it -- the socket table it references
 * was just wiped out from under it). */
static void treset(void)
{
    memset(socks, 0, sizeof socks);
    legacy_dq.used = 0;
    legacy_dq.sock = -1;
}

/* Receive on a socket with the old one-shot API's reading: -1 = nothing. */
static int trecv(int s, uint8_t *buf, int max, uint32_t *src, uint16_t *sport)
{
    int n = udp_recv(s, buf, max, src, sport);
    return n == 0 ? -1 : n;
}

int main(void)
{
    uint8_t frame[1600], datagram[1500], recvbuf[64];
    const uint8_t odd_payload[] = { 'a', 'e', 't', 'h', 'e', 'r', '!' };
    uint32_t rsrc;
    uint16_t rsport;

    /* 1) Outbound UDP carries a valid nonzero pseudo-header checksum. */
    eth_sends = 0;
    CHECK(udp_send(REMOTE_IP, 53, 0x4444, odd_payload, sizeof odd_payload) == 0,
          "UDP send failed");
    struct ip_hdr *wip = (struct ip_hdr *)wire;
    struct udp_hdr *wudp = (struct udp_hdr *)(wire + sizeof *wip);
    uint16_t wulen = ntohs(wudp->length);
    CHECK(eth_sends == 1 && wire_len == sizeof *wip + wulen && wudp->checksum != 0 &&
          udp_checksum(LOCAL_IP, REMOTE_IP, (uint8_t *)wudp, wulen) == 0,
          "UDP send checksum/length invalid");

    /* 2) If the arithmetic checksum is zero, encode it as 0xffff rather than
     * the IPv4 "checksum omitted" marker. */
    int zero_word = -1;
    uint8_t two[2];
    for (int v = 0; v <= 0xFFFF; v++) {
        two[0] = (uint8_t)(v >> 8); two[1] = (uint8_t)v;
        uint16_t n = make_udp(datagram, LOCAL_IP, REMOTE_IP, 1234, 4321, two, 2, 0);
        if (udp_checksum(LOCAL_IP, REMOTE_IP, datagram, n) == 0) { zero_word = v; break; }
    }
    CHECK(zero_word >= 0, "UDP checksum-zero test vector not found");
    two[0] = (uint8_t)(zero_word >> 8); two[1] = (uint8_t)zero_word;
    udp_send(REMOTE_IP, 4321, 1234, two, 2);
    wudp = (struct udp_hdr *)(wire + sizeof(struct ip_hdr));
    CHECK(ntohs(wudp->checksum) == 0xFFFFu, "UDP computed zero encoded as %04x",
          ntohs(wudp->checksum));

    /* 3) Valid inbound UDP dispatches, preserving source IP and port. */
    uint16_t ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444,
                             odd_payload, sizeof odd_payload, 1);
    int flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                          datagram, ulen, 0, 64);
    treset();
    int s0 = udp_bind(0x4444);
    CHECK(s0 >= 0, "udp_bind(0x4444) failed");
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, recvbuf, sizeof recvbuf, &rsrc, &rsport) == (int)sizeof odd_payload &&
          memcmp(recvbuf, odd_payload, sizeof odd_payload) == 0 &&
          rsrc == REMOTE_IP && rsport == 53,
          "UDP valid receive/source metadata");

    /* 4) A corrupt nonzero checksum is dropped; a zero IPv4 checksum remains
     * accepted for compatibility with RFC 768. */
    datagram[sizeof(struct udp_hdr)] ^= 1;
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, recvbuf, sizeof recvbuf, NULL, NULL) == -1,
          "UDP corrupt checksum was not dropped");

    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444,
                    odd_payload, sizeof odd_payload, 0);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, recvbuf, sizeof recvbuf, NULL, NULL) == (int)sizeof odd_payload,
          "UDP zero checksum should be accepted on IPv4");

    /* 5) The reserved fragment flag, zero TTL, wrong destination, and invalid
     * on-wire source forms are rejected before transport dispatch. (MF and
     * nonzero-offset fragments are no longer dropped -- group 8 covers the
     * reassembly they now go through.) */
    const struct { uint16_t frag; uint8_t ttl; uint32_t src, dst; const char *name; } bad_ip[] = {
        { 0x8000, 64, REMOTE_IP, LOCAL_IP, "reserved fragment flag" },
        { 0,       0, REMOTE_IP, LOCAL_IP, "zero TTL" },
        { 0,      64, REMOTE_IP, 0x0A000210u, "wrong destination" },
        { 0,      64, 0x7F000001u, LOCAL_IP, "loopback source on wire" },
    };
    for (unsigned i = 0; i < sizeof bad_ip / sizeof bad_ip[0]; i++) {
        flen = make_frame(frame, bad_ip[i].src, bad_ip[i].dst, IP_PROTO_UDP,
                          datagram, ulen, bad_ip[i].frag, bad_ip[i].ttl);
        ip_input(frame, (uint16_t)flen);
        CHECK(trecv(s0, recvbuf, sizeof recvbuf, NULL, NULL) == -1,
              "IPv4 accepted %s", bad_ip[i].name);
    }

    /* 6) ICMP validates its checksum and returns a checksummed echo reply. */
    uint8_t echo[sizeof(struct icmp_hdr) + 5];
    struct icmp_hdr *eh = (struct icmp_hdr *)echo;
    memset(echo, 0, sizeof echo);
    eh->type = ICMP_ECHO_REQUEST; eh->id = htons(7); eh->seq = htons(9);
    memcpy(echo + sizeof *eh, "ping!", 5);
    eh->checksum = htons(ip_checksum(echo, sizeof echo));
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_ICMP,
                      echo, sizeof echo, 0, 64);
    eth_sends = 0;
    ip_input(frame, (uint16_t)flen);
    wip = (struct ip_hdr *)wire;
    struct icmp_hdr *wic = (struct icmp_hdr *)(wire + sizeof *wip);
    uint16_t wilen = (uint16_t)(ntohs(wip->total_len) - sizeof *wip);
    CHECK(eth_sends == 1 && wic->type == ICMP_ECHO_REPLY &&
          ip_checksum(wic, wilen) == 0, "ICMP echo reply/checksum invalid");

    echo[sizeof echo - 1] ^= 1;
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_ICMP,
                      echo, sizeof echo, 0, 64);
    eth_sends = 0;
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 0, "ICMP corrupt checksum generated a reply");

    /* 7) Ping replies must match destination, identifier, and current sequence. */
    ticks = 100;
    CHECK(icmp_ping(REMOTE_IP) == 0, "icmp_ping send failed");
    wip = (struct ip_hdr *)wire;
    wilen = (uint16_t)(ntohs(wip->total_len) - sizeof *wip);
    uint8_t reply[128];
    memcpy(reply, wire + sizeof *wip, wilen);
    struct icmp_hdr *rh = (struct icmp_hdr *)reply;
    rh->type = ICMP_ECHO_REPLY; rh->checksum = 0;
    rh->checksum = htons(ip_checksum(reply, wilen));
    ticks = 107;
    icmp_input(0x0A000203u, reply, wilen);
    CHECK(icmp_last_rtt() == -1, "ICMP accepted reply from wrong host");
    icmp_input(REMOTE_IP, reply, wilen);
    CHECK(icmp_last_rtt() == 7, "ICMP RTT=%d want 7", icmp_last_rtt());

    /* 8) IPv4 reassembly: two fragments delivered out of order are reassembled
     *    and dispatched as one UDP datagram (checksum re-verified end to end). */
    ticks = 1000;
    uint8_t big[100], bigbuf[128];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (uint8_t)(i * 3 + 1);
    uint16_t blen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444,
                             big, sizeof big, 1);
    frame_id = 0x1001;
    /* tail fragment first: offset 64 bytes = 8 units, MF clear */
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram + 64, (uint16_t)(blen - 64), 8, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, bigbuf, sizeof bigbuf, NULL, NULL) == -1 && used_slots() == 1,
          "reasm: tail fragment alone must wait (slots=%d)", used_slots());
    /* head fragment: offset 0, MF set, 64 bytes */
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, 64, 0x2000, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, bigbuf, sizeof bigbuf, &rsrc, &rsport) == (int)sizeof big &&
          memcmp(bigbuf, big, sizeof big) == 0 &&
          rsrc == REMOTE_IP && rsport == 53 && used_slots() == 0,
          "reasm: out-of-order fragments reassembled");

    /* 9) An overlapping fragment poisons the whole datagram (RFC 5722):
     *    even the honest tail that arrives later cannot complete it. */
    frame_id = 0x1002;
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, 64, 0x2000, 64);
    ip_input(frame, (uint16_t)flen);
    /* overlaps [32,64) of the first fragment */
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram + 32, 64, 0x2000 | 4, 64);
    ip_input(frame, (uint16_t)flen);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram + 64, (uint16_t)(blen - 64), 8, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, bigbuf, sizeof bigbuf, NULL, NULL) == -1,
          "reasm: overlapping fragment must kill the datagram");

    /* 10) A lone fragment times out (~30 s) and its slot is forgotten; the
     *    late tail then starts a new, incomplete datagram. */
    frame_id = 0x1003;
    ticks = 5000;
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, 64, 0x2000, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, bigbuf, sizeof bigbuf, NULL, NULL) == -1 && used_slots() >= 1,
          "reasm: head fragment buffered");
    ticks = 5000 + 3001;                 /* past the ~30 s reassembly timeout */
    ip_poll();
    CHECK(used_slots() == 0, "reasm: stale slot not swept (slots=%d)", used_slots());
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram + 64, (uint16_t)(blen - 64), 8, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, bigbuf, sizeof bigbuf, NULL, NULL) == -1,
          "reasm: timed-out datagram must stay dead");
    ticks += 3001;                       /* sweep the tail's slot too */
    ip_poll();
    CHECK(used_slots() == 0, "reasm: cleanup sweep (slots=%d)", used_slots());

    /* 11) A datagram that would exceed the 64 KiB slot buffer is dropped. */
    frame_id = 0x1004;
    ticks = 12000;
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, 64, 0x2000, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(used_slots() == 1, "reasm: head fragment buffered before oversize");
    /* offset 65520 (8190 units) + 64 bytes > 64 KiB -> poison */
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, 64, 0x2000 | 8190, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(s0, bigbuf, sizeof bigbuf, NULL, NULL) == -1 && used_slots() == 0,
          "reasm: >64 KiB datagram must be dropped (slots=%d)", used_slots());

    /* 12) An ICMP port-unreachable quoting our DNS query fails the waiting
     *    receive socket immediately; one quoting a different port does not. */
    frame_id = 0;
    ticks = 10;
    treset();
    uint8_t ouh[sizeof(struct udp_hdr) + 4];
    dns_start("example.com");            /* binds an ephemeral-port socket */
    CHECK(legacy_dq.sock >= 0, "dns_start did not bind a socket");
    uint16_t dport = socks[legacy_dq.sock].port;
    make_udp(ouh, LOCAL_IP, DNS_IP, dport, 53, (const uint8_t *)"A?", 2, 0);
    uint16_t mlen = make_icmp_error(datagram, 3, 3, LOCAL_IP, DNS_IP,
                                    IP_PROTO_UDP, ouh, sizeof ouh);
    flen = make_frame(frame, DNS_IP, LOCAL_IP, IP_PROTO_ICMP,
                      datagram, mlen, 0, 255);
    ip_input(frame, (uint16_t)flen);
    CHECK(dns_result() == 0xFFFFFFFFu,
          "icmp error: dns_result did not fail fast (got %08x)", dns_result());
    CHECK(legacy_dq.sock < 0, "icmp error: dns socket not released");

    ticks = 10;
    dns_start("example.com");            /* re-arms (fresh socket) */
    CHECK(legacy_dq.sock >= 0, "dns re-start did not bind a socket");
    make_udp(ouh, LOCAL_IP, DNS_IP, 0x4444, 53, (const uint8_t *)"A?", 2, 0);
    mlen = make_icmp_error(datagram, 3, 3, LOCAL_IP, DNS_IP,
                           IP_PROTO_UDP, ouh, sizeof ouh);
    flen = make_frame(frame, DNS_IP, LOCAL_IP, IP_PROTO_ICMP,
                      datagram, mlen, 0, 255);
    ip_input(frame, (uint16_t)flen);
    CHECK(dns_result() == 0,
          "icmp error: error for another port must not fail the lookup");

    /* 13) Multi-socket UDP: two bound ports receive independently and in
     *    order; a full queue drops the newest datagram (counted); a port
     *    with no socket draws a rate-limited ICMP port-unreachable. */
    ticks = 20000;
    treset();
    int sa = udp_bind(0x4444), sb = udp_bind(0x5555);
    CHECK(sa >= 0 && sb >= 0 && udp_bind(0x4444) < 0,
          "bind: two sockets, and rebinding a taken port must fail");
    const uint8_t payA[] = { 'A', '1' }, payB[] = { 'B', '2' }, payB2[] = { 'B', '3' };
    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444, payA, sizeof payA, 1);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP, datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x5555, payB, sizeof payB, 1);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP, datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x5555, payB2, sizeof payB2, 1);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP, datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(trecv(sa, recvbuf, sizeof recvbuf, &rsrc, &rsport) == 2 &&
          memcmp(recvbuf, payA, 2) == 0 && rsport == 53,
          "multi: socket A got its datagram");
    CHECK(trecv(sb, recvbuf, sizeof recvbuf, NULL, NULL) == 2 &&
          memcmp(recvbuf, payB, 2) == 0 &&
          trecv(sb, recvbuf, sizeof recvbuf, NULL, NULL) == 2 &&
          memcmp(recvbuf, payB2, 2) == 0,
          "multi: socket B got both datagrams in order");
    CHECK(trecv(sa, recvbuf, sizeof recvbuf, NULL, NULL) == -1,
          "multi: socket A queue drained");
    /* overflow: 5 datagrams into a 4-slot queue -> 1 drop, order preserved */
    for (int i = 0; i < 5; i++) {
        uint8_t pb = (uint8_t)i;
        ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444, &pb, 1, 1);
        flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP, datagram, ulen, 0, 64);
        ip_input(frame, (uint16_t)flen);
    }
    CHECK(udp_drops(sa) == 1, "overflow: drops=%u want 1", udp_drops(sa));
    int order_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (trecv(sa, recvbuf, sizeof recvbuf, NULL, NULL) != 1 || recvbuf[0] != (uint8_t)i)
            order_ok = 0;
    }
    CHECK(order_ok, "overflow: first four datagrams survived in order");

    /* no socket on 0x9999: rate-limited ICMP port-unreachable */
    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 9999, 0x9999,
                    odd_payload, sizeof odd_payload, 1);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    eth_sends = 0;
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 1, "unreach: expected one ICMP error (sends=%d)", eth_sends);
    wip = (struct ip_hdr *)wire;
    wilen = (uint16_t)(ntohs(wip->total_len) - sizeof *wip);
    wic = (struct icmp_hdr *)(wire + sizeof *wip);
    CHECK(wip->proto == IP_PROTO_ICMP && wic->type == 3 && wic->code == 3 &&
          ip_checksum(wic, wilen) == 0,
          "unreach: not a valid port-unreachable (proto=%d type=%d code=%d)",
          wip->proto, wic->type, wic->code);
    struct ip_hdr *qip = (struct ip_hdr *)(wire + sizeof *wip + sizeof *wic);
    const uint8_t *ql4 = (const uint8_t *)qip + sizeof *qip;
    CHECK(ntohl(qip->src) == REMOTE_IP && ntohl(qip->dst) == LOCAL_IP &&
          qip->proto == IP_PROTO_UDP &&
          ql4[0] == 0x27 && ql4[1] == 0x0F &&     /* sport 9999 */
          ql4[2] == 0x99 && ql4[3] == 0x99,       /* dport 0x9999 */
          "unreach: quote does not match the original packet");
    /* within the same second: suppressed; after it: allowed again */
    ticks = 20050;
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 1, "unreach: second error within 1 s not suppressed");
    ticks = 20100;
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 2, "unreach: error after 1 s was suppressed");
    /* a bound port receives quietly (no error even when the queue is full),
     * and once closed its port goes back to unreachable */
    udp_close(sa);
    ticks = 20200;
    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444,
                    odd_payload, sizeof odd_payload, 1);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 3, "unreach: closed port must be unreachable again");

    /* 14) Broadcast UDP (limited and subnet-directed) is received; broadcast
     *    ICMP echo is not answered; broadcast UDP with no receiver draws no
     *    ICMP error either. */
    treset();
    int sbcast = udp_bind(0x4444);
    CHECK(sbcast >= 0, "bind for broadcast tests failed");
    uint32_t bcasts[] = { 0xFFFFFFFFu, 0x0A0002FFu };
    for (unsigned i = 0; i < sizeof bcasts / sizeof bcasts[0]; i++) {
        ulen = make_udp(datagram, REMOTE_IP, bcasts[i], 53, 0x4444,
                        odd_payload, sizeof odd_payload, 1);
        flen = make_frame(frame, REMOTE_IP, bcasts[i], IP_PROTO_UDP,
                          datagram, ulen, 0, 64);
        ip_input(frame, (uint16_t)flen);
        CHECK(trecv(sbcast, bigbuf, sizeof bigbuf, NULL, NULL) == (int)sizeof odd_payload &&
              memcmp(bigbuf, odd_payload, sizeof odd_payload) == 0,
              "broadcast UDP to %08x dropped", bcasts[i]);
    }
    memset(echo, 0, sizeof echo);
    eh->type = ICMP_ECHO_REQUEST; eh->id = htons(7); eh->seq = htons(9);
    memcpy(echo + sizeof *eh, "ping!", 5);
    eh->checksum = htons(ip_checksum(echo, sizeof echo));
    flen = make_frame(frame, REMOTE_IP, 0xFFFFFFFFu, IP_PROTO_ICMP,
                      echo, sizeof echo, 0, 64);
    eth_sends = 0;
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 0, "broadcast ICMP echo must not be answered");
    ulen = make_udp(datagram, REMOTE_IP, 0xFFFFFFFFu, 9999, 0x9999,
                    odd_payload, sizeof odd_payload, 1);
    flen = make_frame(frame, REMOTE_IP, 0xFFFFFFFFu, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    ip_input(frame, (uint16_t)flen);
    CHECK(eth_sends == 0 && trecv(sbcast, bigbuf, sizeof bigbuf, NULL, NULL) == -1,
          "broadcast UDP with no receiver drew an ICMP error");

    /* 15) ip_send to a broadcast address skips ARP and uses eth_broadcast. */
    eth_sends = 0; arp_calls = 0;
    CHECK(ip_send(0xFFFFFFFFu, IP_PROTO_UDP, odd_payload,
                  sizeof odd_payload) == 0 &&
          eth_sends == 1 && arp_calls == 0 &&
          eth_last_dst[0] == 0xFF && eth_last_dst[5] == 0xFF,
          "broadcast send must use eth_broadcast without ARP");
    CHECK(ip_send(REMOTE_IP, IP_PROTO_UDP, odd_payload,
                  sizeof odd_payload) == 0 && arp_calls == 1,
          "unicast send must still go through ARP");

    printf("\nIPv4/UDP/ICMP protocol tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
