/* Host-side IPv4/UDP/ICMP protocol tests. This is a white-box translation unit:
 * it includes the three implementation files and stubs only ARP, Ethernet, and
 * the timer, so checksums and IP dispatch run exactly as they do in the kernel. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "eth.h"
#include "arp.h"
#include "pit.h"

#define LOCAL_IP 0x0A00020Fu       /* 10.0.2.15 */
#define REMOTE_IP 0x0A000202u      /* 10.0.2.2  */

struct net_config net_cfg = {
    .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .ip = LOCAL_IP, .mask = 0xFFFFFF00u, .gw = REMOTE_IP,
};

static uint8_t wire[1600];
static uint16_t wire_len;
static int eth_sends;
static uint64_t ticks;

uint64_t timer_ticks(void) { return ticks; }
int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    (void)ip;
    memset(mac, 0xAB, ETH_ALEN);
    return 0;
}
int eth_send(const uint8_t dst[ETH_ALEN], uint16_t type,
             const void *payload, uint16_t len)
{
    (void)dst;
    if (type != ETHERTYPE_IP || len > sizeof wire) return -1;
    memcpy(wire, payload, len);
    wire_len = len;
    eth_sends++;
    return 0;
}

/* Avoid fortified memcpy macros conflicting with the kernel prototypes. */
#undef memcpy
#include "ip.c"
#include "udp.c"
#include "icmp.c"

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

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

int main(void)
{
    uint8_t frame[1600], datagram[1500], recvbuf[64];
    const uint8_t odd_payload[] = { 'a', 'e', 't', 'h', 'e', 'r', '!' };

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
    udp_recv_bind(0x4444, recvbuf, sizeof recvbuf);
    ip_input(frame, (uint16_t)flen);
    CHECK(udp_recv_len() == (int)sizeof odd_payload &&
          memcmp(recvbuf, odd_payload, sizeof odd_payload) == 0 &&
          udp_recv_src() == REMOTE_IP && udp_recv_sport() == 53,
          "UDP valid receive/source metadata");

    /* 4) A corrupt nonzero checksum is dropped; a zero IPv4 checksum remains
     * accepted for compatibility with RFC 768. */
    datagram[sizeof(struct udp_hdr)] ^= 1;
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    udp_recv_bind(0x4444, recvbuf, sizeof recvbuf);
    ip_input(frame, (uint16_t)flen);
    CHECK(udp_recv_len() == -1, "UDP corrupt checksum was not dropped");

    ulen = make_udp(datagram, REMOTE_IP, LOCAL_IP, 53, 0x4444,
                    odd_payload, sizeof odd_payload, 0);
    flen = make_frame(frame, REMOTE_IP, LOCAL_IP, IP_PROTO_UDP,
                      datagram, ulen, 0, 64);
    udp_recv_bind(0x4444, recvbuf, sizeof recvbuf);
    ip_input(frame, (uint16_t)flen);
    CHECK(udp_recv_len() == (int)sizeof odd_payload,
          "UDP zero checksum should be accepted on IPv4");

    /* 5) Unsupported IP fragments, zero TTL, wrong destination, and invalid
     * on-wire source forms are rejected before transport dispatch. */
    const struct { uint16_t frag; uint8_t ttl; uint32_t src, dst; const char *name; } bad_ip[] = {
        { 0x2000, 64, REMOTE_IP, LOCAL_IP, "MF fragment" },
        { 0x0001, 64, REMOTE_IP, LOCAL_IP, "offset fragment" },
        { 0,       0, REMOTE_IP, LOCAL_IP, "zero TTL" },
        { 0,      64, REMOTE_IP, 0x0A000210u, "wrong destination" },
        { 0,      64, 0x7F000001u, LOCAL_IP, "loopback source on wire" },
    };
    for (unsigned i = 0; i < sizeof bad_ip / sizeof bad_ip[0]; i++) {
        flen = make_frame(frame, bad_ip[i].src, bad_ip[i].dst, IP_PROTO_UDP,
                          datagram, ulen, bad_ip[i].frag, bad_ip[i].ttl);
        udp_recv_bind(0x4444, recvbuf, sizeof recvbuf);
        ip_input(frame, (uint16_t)flen);
        CHECK(udp_recv_len() == -1, "IPv4 accepted %s", bad_ip[i].name);
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

    printf("\nIPv4/UDP/ICMP protocol tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
