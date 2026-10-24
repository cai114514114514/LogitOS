/* Real TCP and socket consumers, with only NIC/DNS/ARP/time apparatus.
 * A failed connection releases its unowned TCP slot before the broker sees
 * it. Arrange the new socket below the old socket in the broker's walk: a new
 * SYN can then reuse that slot before the old socket's cleanup. The peer's
 * checksum-valid SYN-ACK must receive ACK, not the unmatched-connection RST.
 *
 * No TCP/socket state is injected. The white-box reads identify the slots and
 * protocol transitions produced by open/pump/packet input/close. Each case
 * runs in a fresh process, so production BSS supplies its initial state.
 * This establishes the lifecycle defect; it does not identify every guest
 * transport failure as this defect or measure SMP locking. */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "net.h"
#include "ip6.h"
#include "../../include/weaksym.h"
void tcp_conn_census(char *buf, int cap) LOGIT_WEAK;

struct net_config net_cfg = {
    .ip = 0x0a00020f, .mask = 0xffffff00, .gw = 0x0a000202,
};
static uint64_t ticks = 1;
static int clock_on_syn, clock_reads, clock_steps;
uint64_t timer_ticks(void)
{
    if (clock_reads && --clock_reads == 0) { ticks++; clock_steps++; }
    return ticks;
}
void net_poll(void) {}
void net_idle(void) { ticks++; }
int net_up(void) { return 1; }
void kernel_random_bytes(uint8_t *p, int n)
{ for (int i = 0; i < n; i++) p[i] = (uint8_t)(i + 17); }

static unsigned char captured[1600];
static int captured_len;
static int capture(const void *p, int n)
{
    if (n > (int)sizeof captured) return -1;
    memcpy(captured, p, (size_t)n); captured_len = n;
    if (clock_on_syn && n >= 20 && captured[13] == 0x02) {
        /* The real active open reads rtx_tick and rtt_tick after sending SYN;
         * its broker then reads the CONNECT phase start. Cross exactly at
         * that third read. The case checks both TCP stamps so a changed call
         * sequence cannot silently move this apparatus to a different edge. */
        clock_on_syn = 0;
        clock_reads = 3;
    }
    return 0;
}
int ip_send(uint32_t dst, uint8_t proto, const void *p, uint16_t n)
{ (void)dst; (void)proto; return capture(p, n); }

/* The real implementations below satisfy these optional symbols. Suppress
 * the Mach-O weak fallback definitions, not the actual packet consumers. */
#define LOGIT_WEAK_LOCAL_ip6_send 1
#define LOGIT_WEAK_LOCAL_ip6_local_addr 1
#define LOGIT_WEAK_LOCAL_tcp_conn_census 1
#undef memcpy
#undef memset
#include "transport/tcp.c"
#include "ip/ip6_addr.c"

int ip6_send(const uint8_t dst[16], uint8_t proto, const void *p, uint16_t n)
{ (void)dst; (void)proto; return capture(p, n); }
int ip6_local_addr(const uint8_t dst[16], uint8_t out[16])
{ (void)dst; ip6_addr local; ip6_parse("2001:db8::1", &local); memcpy(out, local.b, 16); return 0; }

#include "rtc.h"
void rtc_now(struct rtc_time *t)
{ memset(t, 0, sizeof *t); t->year = 2026; t->month = 10; t->day = 1; }
void kprintf(const char *fmt, ...)
{ va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); }
int arp_resolve(uint32_t ip, uint8_t mac[6])
{ (void)ip; memset(mac, 0x11, 6); return 0; }
int arp_warm(uint32_t ip, int timeout) { (void)ip; (void)timeout; return 0; }
int tls_send(int s, const void *b, int n) { (void)s; (void)b; return n; }
int tls_recv(int s, void *b, int n) { (void)s; (void)b; (void)n; return 0; }
void tls_close(int s) { (void)s; }

static int resolver_used[8], next_query, new_ready, ipv6;
int dns_query_start(const char *name)
{
    (void)name;
    if (next_query >= 8) return -1;
    int id = next_query++; resolver_used[id] = 1;
    return id;
}
void dns_query_free(int id) { resolver_used[id] = 0; }
int dns_query_addrs(int id, ip6_addr *out, int max)
{
    if (!resolver_used[id] || max < 1) return -1;
    if (id == 0 && !new_ready) return 0;
    if (ipv6) ip6_parse("2001:db8::2", out);
    else ip6_from_v4(0x0a000202, out);
    return 1;
}
#include "core/sock.c"

static const char *case_name;
static int checks, failures;
static void check(int ok, const char *label)
{
    checks++;
    if (!ok) { failures++; printf("FAIL: %s: %s\n", case_name, label); }
}

struct pair { int waiting, old, old_tcp; };
struct channel { struct tcp_addr remote, local; uint16_t port; uint32_t ack; };
static struct channel channel_of(int id)
{
    return (struct channel){ conns[id].raddr, conns[id].laddr,
                             conns[id].lport, conns[id].snd_nxt };
}
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void packet(const struct channel *c, uint32_t seq, uint32_t ack, uint8_t flags)
{
    uint8_t p[20] = {0};
    p[1] = 80; p[2] = c->port >> 8; p[3] = c->port;
    put32(p + 4, seq); put32(p + 8, ack);
    p[12] = 0x50; p[13] = flags; p[14] = p[15] = 0xff;
    uint16_t sum = tcp_checksum_af(&c->remote, &c->local, p, sizeof p);
    p[16] = sum >> 8; p[17] = sum;
    tcp_input_af(&c->remote, &c->local, p, sizeof p);
}

static int open_pair(struct pair *p)
{
    p->waiting = sock_open("pending", 80, 0, 1);
    p->old = sock_open("first", 80, 0, 1);
    check(p->waiting == 0 && p->old == 1, "fresh socket handles allocate in walk order");
    if (p->waiting < 0 || p->old < 0) return 0;
    sock_pump(); p->old_tcp = socks[p->old].tcp;
    check(p->old_tcp >= 0 && conns[p->old_tcp].state == SYN_SENT &&
          captured_len >= 20 && captured[13] == SYN,
          "old socket reached actual SYN_SENT");
    return p->old_tcp >= 0;
}
static int release_waiter(const struct pair *p)
{
    new_ready = 1; sock_pump();
    int id = socks[p->waiting].tcp;
    if (id < 0) check(0, "apparatus must allocate the new TCP dial");
    return id;
}
static void check_handshake(const struct pair *p, int id, const struct channel *wire)
{
    captured_len = 0; packet(wire, 64001, wire->ack, SYN | ACK);
    unsigned flags = captured_len >= 20 ? captured[13] : 0;
    printf("%s new SYN-ACK response: flags=0x%02x tcp=%d used=%d state=%d\n",
           case_name, flags, id, conns[id].used, conns[id].state);
    check(flags == ACK && conns[id].used && conns[id].state == ESTABLISHED,
          "valid new SYN-ACK receives ACK instead of guest RST");
    sock_pump(); int bits = sock_poll_bits(p->waiting, 1);
    check(bits > 0 && (bits & SOCK_P_CONNECTED),
          "new socket connects through the actual consumer");
}
static void cleanup(void)
{
    sock_close_owner(1);
    int pinned = 0;
    for (int i = 0; i < NCONN; i++) pinned += conns[i].used && conns[i].app_owned;
    check(!pinned, "owner cleanup releases every live TCP pin");
}

static void failed_open(int icmp)
{
    struct pair p;
    if (!open_pair(&p)) return;
    struct channel old_wire = channel_of(p.old_tcp);
    if (icmp) tcp_error(old_wire.port, 0x0a000202, 80, 3, 1);
    else packet(&old_wire, 0, old_wire.ack, RST | ACK);
    printf("%s terminal: sock=%d tcp=%d used=%d owned=%d state=%d\n", case_name,
           p.old, p.old_tcp, conns[p.old_tcp].used, conns[p.old_tcp].app_owned, conns[p.old_tcp].state);
    check(conns[p.old_tcp].used && conns[p.old_tcp].state == CLOSED,
          "failed TCP slot stays pinned until socket cleanup");
    int next = release_waiter(&p);
    if (next < 0) return;
    struct channel new_wire = channel_of(next);
    printf("%s new dial: sock=%d tcp=%d port=%u old_tcp=%d\n",
           case_name, p.waiting, next, new_wire.port, p.old_tcp);
    check(next != p.old_tcp, "new socket cannot reuse a failed owned TCP slot");
    sock_close(p.old, 1);
    check(conns[next].used && conns[next].state == SYN_SENT,
          "old socket cleanup cannot close the new dial");
    check_handshake(&p, next, &new_wire);
    cleanup();
}

static void orderly_close(void)
{
    struct pair p;
    if (!open_pair(&p)) return;
    struct channel old_wire = channel_of(p.old_tcp);
    packet(&old_wire, 100, old_wire.ack, SYN | ACK); sock_pump();
    int bits = sock_poll_bits(p.old, 1);
    check(bits > 0 && (bits & SOCK_P_CONNECTED), "old socket completed the actual handshake");
    packet(&old_wire, 101, old_wire.ack, FIN | ACK);
    check(conns[p.old_tcp].used && conns[p.old_tcp].state == CLOSE_WAIT,
          "peer FIN retains the TCP slot while the socket is open");
    char byte;
    check(sock_recv(p.old, &byte, 1, 1) < 0 && conns[p.old_tcp].used,
          "reading peer EOF does not release the open socket's slot");
    int next = release_waiter(&p);
    if (next < 0) return;
    struct channel new_wire = channel_of(next);
    check(next != p.old_tcp, "new SYN before local close uses a different slot");
    sock_close(p.old, 1);
    check(sock_poll_bits(p.old, 1) == SOCK_E_ARG, "local close removes the old socket handle");
    check(conns[p.old_tcp].used && conns[p.old_tcp].state == LAST_ACK,
          "normal local close leaves its FIN awaiting the peer's final ACK");
    packet(&old_wire, 102, conns[p.old_tcp].snd_nxt, ACK);
    check(!conns[p.old_tcp].used, "final FIN ACK releases the formally closed TCP slot");
    check_handshake(&p, next, &new_wire);
    int third = sock_open("after-close", 80, 0, 1); sock_pump();
    check(third >= 0 && socks[third].tcp == p.old_tcp && conns[next].state == ESTABLISHED,
          "released old slot can be reused without harming the live connection");
    cleanup();
}

static void clock_boundary(void)
{
    new_ready = 1;
    clock_on_syn = 1;
    int fd = sock_open("clock-boundary", 80, 0, 1);
    check(fd == 0, "clock case allocates a fresh socket handle");
    if (fd < 0) return;
    sock_pump();
    /* Fresh BSS allocates TCP slot zero. Read its wire tuple even if the old
     * broker already released it: injecting a valid peer reply after that
     * premature release is precisely the observed guest RST boundary. */
    struct channel wire = channel_of(0);
    check(captured_len >= 20 && captured[13] == SYN && wire.port >= 49152,
          "clock case emits a real TCP SYN");
    check(clock_steps == 1 && ticks == 2 && conns[0].rtx_tick == 1 &&
          conns[0].rtt_tick == 1,
          "apparatus crosses the tick after both TCP timer stamps");
    check(socks[fd].t0 == 2, "CONNECT start records the new tick");
    int bits = sock_poll_bits(fd, 1);
    printf("%s after tick: poll=%d error=%d sock_state=%d tcp_used=%d tcp_state=%d\n",
           case_name, bits, socks[fd].err, socks[fd].state,
           conns[0].used, conns[0].state);
    check(bits == 0 && socks[fd].state == S_CONNECT,
          "one tick cannot expire the new socket connection");
    check(conns[0].used && conns[0].state == SYN_SENT,
          "clock boundary keeps the new TCP SYN pending");
    struct pair p = { .waiting = fd };
    check_handshake(&p, 0, &wire);
    cleanup();
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "rst4")) case_name = "RST IPv4";
    else if (!strcmp(argv[1], "rst6")) { case_name = "RST IPv6"; ipv6 = 1; }
    else if (!strcmp(argv[1], "icmp4")) case_name = "ICMP IPv4";
    else if (!strcmp(argv[1], "fin4")) case_name = "FIN IPv4";
    else if (!strcmp(argv[1], "fin6")) { case_name = "FIN IPv6"; ipv6 = 1; }
    else if (!strcmp(argv[1], "clock4")) case_name = "Clock IPv4";
    else if (!strcmp(argv[1], "clock6")) { case_name = "Clock IPv6"; ipv6 = 1; }
    else return 2;
    if (!strncmp(argv[1], "clock", 5)) clock_boundary();
    else if (!strncmp(argv[1], "fin", 3)) orderly_close();
    else failed_open(!strcmp(argv[1], "icmp4"));
    printf("sock-tcb: %s: %d checks, %d failures\n", case_name, checks, failures);
    return failures ? 1 : 0;
}
