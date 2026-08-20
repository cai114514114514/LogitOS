/* Host-side ICMP raw-socket tests. White-box, same shape as
 * tests/unit/net_proto_test.c: #includes ip.c/reasm.c/icmp.c/raw.c directly
 * and stubs only ARP, Ethernet, the RNG and the timer, so a delivered
 * message runs the REAL ip_input() -> icmp_input() -> raw_icmp_deliver()
 * path rather than a hand-rolled substitute of it. Built with
 * -DLOGIT_NET_HOST so net_lock() degenerates to a no-op.
 *
 * WHAT THIS FILE DOES NOT COVER. c/net/core/lsock.c's LOGIT_SOCK_RAW branch
 * -- the fd/syscall glue and the root-only privilege check against
 * c/fs/vfs_cred.c -- is not included here: lsock.c pulls in file.c, proc.c
 * and vfs_cred.c, i.e. most of the kernel's process model, which is not a
 * host-testable protocol unit the way this file's four TUs are. See
 * tests/net.mk for the on-device check that exercises that half instead.
 * This file is the protocol layer underneath it: the per-socket queue, the
 * fan-out to every open socket, and the real send/receive path through
 * ip_send() / ip_input() / icmp_input(). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "eth.h"
#include "arp.h"
#include "pit.h"

#define LOCAL_IP  0x0A00020Fu       /* 10.0.2.15 */
#define REMOTE_IP 0x0A000202u       /* 10.0.2.2  */

struct net_config net_cfg = {
    .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .ip = LOCAL_IP, .mask = 0xFFFFFF00u, .gw = REMOTE_IP, .dns = REMOTE_IP,
};

static uint8_t wire[1600];
static uint16_t wire_len;
static int eth_sends;
static uint64_t ticks;

uint64_t timer_ticks(void) { return ticks; }
int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{ (void)ip; memset(mac, 0xAB, ETH_ALEN); return 0; }
int arp_warm(uint32_t ip, int timeout) { (void)ip; (void)timeout; return 0; }
int arp_output(uint32_t nexthop, uint16_t ethertype, const void *payload, uint16_t len)
{
    uint8_t mac[ETH_ALEN];
    (void)arp_resolve(nexthop, mac);
    return eth_send(mac, ethertype, payload, len);
}
/* Same reason as net_proto_test.c: ip6.c/nd.c drag in the whole kernel
 * network layer for a file whose point is to be a small white-box unit. */
int ip6_up(void) { return 0; }
struct ip6_src_cand;
int ip6_dual_candidates(struct ip6_src_cand *cand, int max) { (void)cand; (void)max; return 0; }
int eth_send(const uint8_t dst[ETH_ALEN], uint16_t type,
            const void *payload, uint16_t len)
{
    if (type != ETHERTYPE_IP || len > sizeof wire) return -1;
    (void)dst;
    memcpy(wire, payload, len);
    wire_len = len;
    eth_sends++;
    return 0;
}
const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
void net_poll(void) {}
void net_idle(void) {}
void kernel_random_bytes(uint8_t *out, int len)
{ for (int i = 0; i < len; i++) out[i] = (uint8_t)(0x5Au + (unsigned)i); }
/* ip.c now consults a real routing table (c/net/core/route.c) instead of one
 * ternary over net_cfg.mask, and prints the table's boot evidence through
 * kprintf when it changes -- both are new dependencies this file did not
 * have when it was written; see tests/unit/ip_route_test.c, which linked
 * route.c first and is the reference for this stub. netdev_primary_ifindex/
 * netdev_loopback_ifindex are WEAK in ip.c and deliberately left undefined
 * here, same as that file: it is what proves route.h's fallback constants
 * (RT_OIF_NIC0/RT_OIF_LO) are the ones ip.c actually uses with no driver
 * layer linked. */
void kprintf(const char *fmt, ...) { (void)fmt; }

/* Avoid fortified memcpy macros conflicting with the kernel prototypes. */
#undef memcpy
#undef memset
#include "route.c"
#include "ip.c"
#include "reasm.c"
#include "icmp.c"
#include "raw.c"

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A full Ethernet+IP+ICMP echo (request or reply) frame, checksummed at both
 * layers, ready for ip_input(). */
static int make_icmp_echo(uint8_t *frame, uint32_t src, uint32_t dst,
                          int type, uint16_t id, uint16_t seq,
                          const uint8_t *payload, uint16_t plen)
{
    uint8_t icmp[8 + 64];
    uint16_t ilen = (uint16_t)(8 + plen);
    icmp[0] = (uint8_t)type; icmp[1] = 0;
    icmp[2] = 0; icmp[3] = 0;
    icmp[4] = (uint8_t)(id >> 8);  icmp[5] = (uint8_t)id;
    icmp[6] = (uint8_t)(seq >> 8); icmp[7] = (uint8_t)seq;
    memcpy(icmp + 8, payload, plen);
    uint16_t sum = ip_checksum(icmp, ilen);
    icmp[2] = (uint8_t)(sum >> 8); icmp[3] = (uint8_t)sum;

    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memset(eh, 0, sizeof *eh);
    eh->ethertype = htons(ETHERTYPE_IP);
    struct ip_hdr *ih = (struct ip_hdr *)(frame + sizeof *eh);
    memset(ih, 0, sizeof *ih);
    ih->ver_ihl = 0x45;
    ih->total_len = htons((uint16_t)(sizeof *ih + ilen));
    ih->ttl = 64;
    ih->proto = IP_PROTO_ICMP;
    ih->src = htonl(src); ih->dst = htonl(dst);
    ih->checksum = htons(ip_checksum(ih, sizeof *ih));
    memcpy((uint8_t *)ih + sizeof *ih, icmp, ilen);
    return (int)(sizeof *eh + sizeof *ih + ilen);
}

int main(void)
{
    uint8_t frame[256], rbuf[128];
    uint8_t payload[16];
    for (int i = 0; i < 16; i++) payload[i] = (uint8_t)i;

    /* 1) The table starts empty, hands out ids in order, and refuses a
     *    (RAW_MAX + 1)th socket rather than silently reusing one that is
     *    still open (nothing here is stubbed to success). */
    int ids[RAW_MAX];
    for (int i = 0; i < RAW_MAX; i++) {
        ids[i] = raw_icmp_open(100 + i);
        CHECK(ids[i] == i, "raw_icmp_open[%d] = %d, want %d", i, ids[i], i);
    }
    CHECK(raw_icmp_open(999) == -1,
          "a (RAW_MAX+1)th raw socket should be refused (RAW_MAX=%d)", RAW_MAX);

    /* 2) raw_icmp_send() hands the caller's ICMP message straight to
     *    ip_send() and adds nothing but the IP header -- proto ICMP, our
     *    address, a valid IP checksum, and the ICMP bytes reaching the wire
     *    UNCHANGED. This is the IP_HDRINCL-is-refused contract: if send ever
     *    started rewriting or re-checksumming the ICMP part itself, this is
     *    what would catch it. */
    uint8_t msg[8] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1 };   /* type=8 id=0x1234 seq=1 */
    uint16_t cksum = ip_checksum(msg, sizeof msg);
    msg[2] = (uint8_t)(cksum >> 8); msg[3] = (uint8_t)cksum;
    eth_sends = 0;
    int rc = raw_icmp_send(ids[0], REMOTE_IP, msg, sizeof msg);
    CHECK(rc == (int)sizeof msg, "raw_icmp_send returned %d, want %d", rc, (int)sizeof msg);
    CHECK(eth_sends == 1, "raw_icmp_send did not reach the wire");
    struct ip_hdr *wip = (struct ip_hdr *)wire;
    CHECK(wip->proto == IP_PROTO_ICMP, "wire proto = %d, want IP_PROTO_ICMP", wip->proto);
    CHECK(ntohl(wip->dst) == REMOTE_IP, "wire dst wrong");
    CHECK(ip_checksum(wip, (wip->ver_ihl & 0xF) * 4) == 0, "wire IP header checksum invalid");
    CHECK(wire_len == sizeof(struct ip_hdr) + sizeof msg &&
          memcmp(wire + sizeof(struct ip_hdr), msg, sizeof msg) == 0,
          "wire ICMP bytes do not match what raw_icmp_send was given");

    /* 3) FAN-OUT: a real inbound echo reply, through the ACTUAL ip_input()
     *    -> icmp_input() -> raw_icmp_deliver() path, reaches EVERY open raw
     *    socket with its own identical copy -- not just the first one asked.
     *    RAW_NEGCTL_FIRST_ONLY (make test-raw-negctl) breaks exactly this. */
    int flen = make_icmp_echo(frame, REMOTE_IP, LOCAL_IP, ICMP_ECHO_REPLY,
                              0x1234, 1, payload, sizeof payload);
    ip_input(frame, (uint16_t)flen);
    int fanout_ok = 1;
    for (int i = 0; i < RAW_MAX; i++) {
        uint32_t rsrc = 0;
        int n = raw_icmp_recv(ids[i], rbuf, sizeof rbuf, &rsrc);
        if (n != 8 + (int)sizeof payload || rsrc != REMOTE_IP ||
            rbuf[0] != ICMP_ECHO_REPLY || rbuf[4] != 0x12 || rbuf[5] != 0x34 ||
            rbuf[7] != 1 || memcmp(rbuf + 8, payload, sizeof payload) != 0)
            fanout_ok = 0;
    }
    CHECK(fanout_ok,
          "every open raw socket should receive its own identical copy of the inbound message");
    uint32_t drained_src = 0;
    CHECK(raw_icmp_recv(ids[0], rbuf, sizeof rbuf, &drained_src) == 0,
          "a second recv with nothing left queued should return 0, not stale data");

    /* 4) Queue-full: RAW_QUEUES+2 messages to one socket nobody drains -- the
     *    newest ones are dropped, the first RAW_QUEUES are kept IN ORDER
     *    (mirrors udp.c's "newest loses" rule, tested the same way
     *    net_proto_test.c tests udp.c's drop counter). Also exercises that a
     *    closed slot's id is reusable. */
    raw_icmp_close(ids[1]);
    int fresh = raw_icmp_open(500);
    CHECK(fresh == 1, "reopening a closed slot should reuse its id (got %d)", fresh);
    for (int i = 0; i < RAW_QUEUES + 2; i++) {
        int fl = make_icmp_echo(frame, REMOTE_IP, LOCAL_IP, ICMP_ECHO_REPLY,
                                0x9999, (uint16_t)i, payload, 1);
        ip_input(frame, (uint16_t)fl);
    }
    /* Every OTHER open socket (ids[0], ids[2..RAW_MAX-1]) also received all
     * RAW_QUEUES+2 of these -- drain them so they do not carry stale state
     * into a future check in this file. */
    for (int i = 0; i < RAW_MAX; i++) {
        if (i == 1) continue;                 /* closed, then reopened as `fresh` */
        uint32_t s = 0;
        while (raw_icmp_recv(ids[i], rbuf, sizeof rbuf, &s) > 0) { }
    }
    int got_count = 0, order_ok = 1;
    for (int i = 0; i < RAW_QUEUES + 2; i++) {
        uint32_t s = 0;
        int rn = raw_icmp_recv(fresh, rbuf, sizeof rbuf, &s);
        if (rn <= 0) break;
        got_count++;
        if (rbuf[7] != (uint8_t)i) order_ok = 0;   /* seq low byte == send order */
    }
    CHECK(got_count == RAW_QUEUES, "queue-full drop kept %d messages, want RAW_QUEUES=%d",
          got_count, RAW_QUEUES);
    CHECK(order_ok, "queue-full drop did not keep the FIRST RAW_QUEUES messages in order");

    /* 5) A closed socket's id cannot be recv'd from -- it is a bad handle,
     *    same convention as udp_recv() on a closed socket. */
    raw_icmp_close(fresh);
    CHECK(raw_icmp_recv(fresh, rbuf, sizeof rbuf, NULL) == -1,
          "recv on a closed raw socket should report -1, not 0 or stale data");

    printf("raw_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
