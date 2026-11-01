#include <stdint.h>
#include <stddef.h>
#include "dhcp.h"
#include "../../drivers/core/io_domain.h"
static struct io_domain dhcp_owner = IO_DOMAIN_INIT;
static unsigned dhcp_runner;
static void dhcp_runner_drop(unsigned *claim)
{ if (*claim) __atomic_store_n(&dhcp_runner, 0, __ATOMIC_RELEASE); }
#include "udp.h"
#include "net.h"
#include "pit.h"
#include "rng.h"
#include "ktime.h"
#include "kprintf.h"
#include "../../../include/weaksym.h"

/* The bring-up's NIC interrupt counter, for the progress line; absent when
 * the driver is not linked (X79-VG4, 2026-10-31). */
extern uint64_t rtl8169_irq_count LOGIT_WEAK;
LOGIT_WEAK_STUB(rtl8169_irq_count);

/* The two hooks this client needs from the layers around it, weak for the
 * reason net.c gives for its own: dhcp.c is built into host tests that have
 * neither a NIC driver nor an ARP layer. netdev_link_up() is the primary
 * interface's link state (c/drivers/net/netdev.c); arp_announce() is the RFC
 * 5227 broadcast net_init() sends after the boot lease, which a lease taken
 * later has to send for itself. */
int  netdev_link_up(void) LOGIT_WEAK;
void arp_announce(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(netdev_link_up);
LOGIT_WEAK_STUB(arp_announce);

#ifdef LOGIT_NET_HOST
static void dhcp_wait(void) { net_idle(); }
#else
/* pause, not hlt: a halt waits for an interrupt that may never come */
static void dhcp_wait(void) { for (int i = 0; i < 2000; i++) __asm__ volatile ("sti; pause"); }
#endif

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* A minimal DHCPv4 client (RFC 2131/2132): one interface, broadcast-only
 * (this stack cannot receive unicast to an address it does not own yet, so
 * every message goes out with the BOOTP broadcast flag set). Lease renewal is
 * a T1 REQUEST; on NAK or repeated silence it falls back to DISCOVER.
 *
 * Two phases. dhcp_run() is the boot attempt: synchronous, bounded, and
 * 1 s between retransmits. If it does not end in a lease the client does NOT
 * stop (X79-VG4, 2026-11-01: the router's link came up 2.5 s after the NIC,
 * the boot attempt had already spent its 3 s, and the machine ran the whole
 * session with the QEMU address on a real LAN). It carries on from
 * dhcp_poll(), which net_poll() reaches from the window manager loop, with
 * the RFC 2131 4.1 backoff, and it follows the link: every down -> up
 * transition restarts the acquisition at once. */

#define DHCP_SPORT 68
#define DHCP_DPORT 67
#define DHCP_MAGIC 0x63825363u

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_RETRY_TICKS 100        /* ~1 s between retransmits */
#define DHCP_MAX_RETRIES 4

/* How long the boot attempt waits for the PHY to finish negotiating before it
 * gives up on the wire and lets the boot continue. By the TSC wall clock, like
 * the exchange bound below, so a stopped timer interrupt cannot stretch it.
 * Gigabit autonegotiation took ~2.5 s on the X79-VG4; 6 s covers a slow switch
 * port without making an unplugged machine wait long. */
#define DHCP_LINK_WAIT_NS 6000000000ull

/* RFC 2131 4.1: "the retransmission delay SHOULD be doubled with subsequent
 * retransmissions up to a maximum of 64 seconds ... The delay before the first
 * retransmission SHOULD be 4 seconds randomized by the value of a uniform
 * random number chosen from the range -1 to +1." Delays 4, 8, 16, 32, 64, 64... */
#define DHCP_BACKOFF_FIRST_SEC  4
#define DHCP_BACKOFF_LAST_SEC   64
#define DHCP_BACKOFF_JITTER_MS  1000

/* Messages sent in REQUESTING or INIT-REBOOT before the client gives up on
 * that server and starts over from DISCOVER: the first, and one at +4 s. */
#define DHCP_REQUEST_TRIES 2

struct bootp_hdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} __attribute__((packed));          /* 240 bytes; options follow on the wire */

/* ST_INIT_REBOOT is RFC 2131 section 3.2's INIT-REBOOT state (figure 5): a
 * client that remembers a lease asks to keep it, with no server identifier. */
enum { ST_IDLE, ST_SELECTING, ST_REQUESTING, ST_BOUND, ST_RENEWING, ST_INIT_REBOOT };

static int      st = ST_IDLE;
static int      sock = -1;
static uint32_t xid;
static uint64_t st_tick;            /* start of the current wait */
static int      retries;
static int      initial;            /* inside dhcp_run(): failure ends the wait */
static uint32_t offered;            /* yiaddr from the OFFER being requested */
static uint32_t server_id;
static uint64_t renew_at;           /* T1: start renewing */
static int      have_lease;         /* net_cfg holds an address a server ACKed us */
static int      link_was_up;        /* the link state the client last acted on */
static uint64_t next_send_ns;       /* background: when the next retransmit is due */
static uint64_t acquire_start_ns;   /* when this acquisition began (boot or link up) */

static uint32_t get32n(const uint8_t *p)    /* network-order 4 bytes -> host */
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Find option `code`; returns a pointer to its value (>= min_len bytes). */
static const uint8_t *opt_find(const uint8_t *o, int len, uint8_t code,
                               uint8_t min_len)
{
    for (int i = 0; i < len;) {
        if (o[i] == 0) { i++; continue; }           /* pad */
        if (o[i] == 255) break;                     /* end */
        if (i + 2 > len || i + 2 + o[i + 1] > len) break;
        if (o[i] == code && o[i + 1] >= min_len)
            return &o[i + 2];
        i += 2 + o[i + 1];
    }
    return NULL;
}

/* Whether the wire is usable right now. A NIC driver that does not report its
 * link (everything but the rtl8169 today) is treated as up, which is what the
 * stack believed about every NIC before link state existed. */
static int dhcp_link_up(void)
{
    if (!LOGIT_HAVE(netdev_link_up))
        return 1;
    return netdev_link_up();
}

/* Choose a transaction id. RFC 2131 4.4.1: a client picks xid for a new
 * exchange and keeps it across the retransmissions of that exchange, so a late
 * reply to an earlier copy of the DISCOVER still matches. A new acquisition
 * (boot, link up, a restart after silence) is a new exchange and gets a new
 * one, which also stops a stale reply from the old network being accepted. */
static void dhcp_new_xid(void)
{
    kernel_random_bytes((uint8_t *)&xid, sizeof xid);
    xid ^= (uint32_t)timer_ticks();
}

/* The delay before retransmission number `attempt` (0 = the first), RFC 2131
 * 4.1: 4 s doubled each time up to 64 s, randomized by -1..+1 s so a segment
 * full of clients that lost power together does not retransmit in step. */
static uint64_t dhcp_backoff_ns(unsigned attempt)
{
    unsigned seconds = DHCP_BACKOFF_FIRST_SEC;
    for (unsigned doubling = 0; doubling < attempt && seconds < DHCP_BACKOFF_LAST_SEC; doubling++)
        seconds *= 2;
    uint16_t random_word = 0;
    kernel_random_bytes((uint8_t *)&random_word, sizeof random_word);
    int64_t jitter_ms = (int64_t)(random_word % (2 * DHCP_BACKOFF_JITTER_MS + 1)) - DHCP_BACKOFF_JITTER_MS;
    int64_t delay_ms = (int64_t)seconds * 1000 + jitter_ms;
    return (uint64_t)delay_ms * 1000000ull;
}

/* Build a client message into out (>= 300 bytes). msgtype is DISCOVER or
 * REQUEST; req_ip/sid (host order, 0 = omit) become options 50/54. */
static int build_msg(uint8_t *out, uint8_t msgtype,
                     uint32_t req_ip, uint32_t sid)
{
    struct bootp_hdr *m = (struct bootp_hdr *)out;
    memset(out, 0, 300);
    m->op = 1;                      /* BOOTREQUEST */
    m->htype = 1;                   /* Ethernet */
    m->hlen = 6;
    m->xid = htonl(xid);
    m->flags = htons(0x8000);       /* broadcast replies, please */
    if (st == ST_RENEWING)
        m->ciaddr = htonl(net_cfg.ip);
    memcpy(m->chaddr, net_cfg.mac, 6);
    m->magic = htonl(DHCP_MAGIC);
    uint8_t *o = out + sizeof *m;
    *o++ = 53; *o++ = 1; *o++ = msgtype;
    if (msgtype == DHCP_DISCOVER) {         /* parameter request list */
        *o++ = 55; *o++ = 4;
        *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    }
    if (req_ip) {
        uint32_t v = htonl(req_ip);
        *o++ = 50; *o++ = 4; memcpy(o, &v, 4); o += 4;
    }
    if (sid) {
        uint32_t v = htonl(sid);
        *o++ = 54; *o++ = 4; memcpy(o, &v, 4); o += 4;
    }
    *o++ = 255;
    return 300;                     /* RFC 2131 minimum BOOTP message size */
}

static void send_discover(void)
{
    uint8_t pkt[300];
    int n = build_msg(pkt, DHCP_DISCOVER, 0, 0);
    udp_send_to(sock, 0xFFFFFFFFu, DHCP_DPORT, pkt, (uint16_t)n);
}

static void send_request(void)
{
    uint8_t pkt[300];
    uint32_t req_ip = 0, sid = 0;
    if (st == ST_SELECTING || st == ST_REQUESTING) {
        req_ip = offered;           /* selecting: name the offer we take */
        sid = server_id;
    } else if (st == ST_INIT_REBOOT) {
        /* RFC 2131 3.2 and 4.3.2: INIT-REBOOT carries the address we remember
         * in option 50, MUST NOT carry a server identifier and has ciaddr 0.
         * Any server that knows the lease answers, so a router that was
         * replaced while the cable was out can still say NAK. */
        req_ip = net_cfg.ip;
    }                               /* renewing: ciaddr carries our address */
    int n = build_msg(pkt, DHCP_REQUEST, req_ip, sid);
    udp_send_to(sock, 0xFFFFFFFFu, DHCP_DPORT, pkt, (uint16_t)n);
}

/* A dotted quad for kprintf. Every log line in this file is ONE kprintf call:
 * the background client runs on the window manager thread while other cores
 * print (a lease line was seen with an unrelated "[sched]" line spliced into
 * the middle of it, and the serial gate that reads it missed it), and kprintf
 * is atomic per call, not per line. */
#define DHCP_IP_FMT "%u.%u.%u.%u"
#define DHCP_IP_ARGS(ip) (unsigned)(((ip) >> 24) & 0xFF), (unsigned)(((ip) >> 16) & 0xFF), \
                         (unsigned)(((ip) >> 8) & 0xFF), (unsigned)((ip) & 0xFF)

/* Adopt an ACKed lease: fill net_cfg and arm the T1 renewal deadline. */
static void apply_lease(const struct bootp_hdr *h,
                        const uint8_t *opts, int olen)
{
    NET_GUARD; /* Lease publication is coherent with packet routing. */
    const uint8_t *v;
    /* The new configuration is composed on the side and stored in one run at
     * the end. Sockets exist by the time a late lease lands, and routing reads
     * net_cfg on every send: a half-built one (mask zeroed, "everything is
     * on-link") must never be visible, not even for the length of this
     * function. */
    struct net_config next = net_cfg;
    /* A lease for a different address than the one we hold is a different
     * network (the static fallback, an old lease, another router). Its mask,
     * gateway and resolver must not survive into the new one when the new
     * server omits an option -- the boot path starts from all zero and so does
     * this. A renewal of the same address keeps them: our REQUEST carries no
     * parameter request list, so a server may legitimately answer with fewer
     * options than the OFFER had (RFC 2131 4.3.1). */
    if (next.ip != ntohl(h->yiaddr)) {
        next.mask = 0;
        next.gw = 0;
        next.dns = 0;
    }
    next.ip = ntohl(h->yiaddr);
    if ((v = opt_find(opts, olen, 1, 4))) next.mask = get32n(v);
    if ((v = opt_find(opts, olen, 3, 4))) next.gw   = get32n(v);
    if ((v = opt_find(opts, olen, 6, 4))) next.dns  = get32n(v);
    uint32_t lease = 3600;                  /* default when option 51 is absent */
    if ((v = opt_find(opts, olen, 51, 4))) lease = get32n(v);
    uint64_t lt = (uint64_t)lease * 100;    /* seconds -> 100 Hz ticks */
    renew_at = timer_ticks() + lt / 2;
    net_cfg.mask = next.mask;
    net_cfg.gw = next.gw;
    net_cfg.dns = next.dns;
    net_cfg.ip = next.ip;

    int plen = 0;
    for (uint32_t m = net_cfg.mask; m & 0x80000000u; m <<= 1) plen++;
    kprintf("[dhcp] bound " DHCP_IP_FMT "/%d gw " DHCP_IP_FMT " dns " DHCP_IP_FMT "\n",
            DHCP_IP_ARGS(net_cfg.ip), plen,
            DHCP_IP_ARGS(net_cfg.gw), DHCP_IP_ARGS(net_cfg.dns));
}

static void enter(int new_st)
{
    st = new_st;
    st_tick = timer_ticks();
    retries = 0;
    /* The caller has just sent the message this state waits on, so the first
     * retransmission is one RFC 2131 4.1 backoff step away. Only the
     * background client reads this; the boot attempt counts ticks. */
    next_send_ns = time_mono_ns() + dhcp_backoff_ns(0);
}

/* The one line a late lease owes the log, so the serial capture of a machine
 * that booted without a lease says when and how it got one. `how` names the
 * path: a full DISCOVER exchange, or INIT-REBOOT confirming the old lease. */
static void dhcp_report_background_lease(const char *how)
{
    unsigned long elapsed_ms = (unsigned long)((time_mono_ns() - acquire_start_ns) / 1000000ull);
    kprintf("[dhcp] lease " DHCP_IP_FMT " gw " DHCP_IP_FMT " dns " DHCP_IP_FMT " after %lu ms (%s)\n",
            DHCP_IP_ARGS(net_cfg.ip), DHCP_IP_ARGS(net_cfg.gw), DHCP_IP_ARGS(net_cfg.dns),
            elapsed_ms, how);
}

/* Take the ACK that ends REQUESTING, RENEWING or INIT-REBOOT. At boot the
 * caller (net_init) finishes the job -- the ARP announcement -- so this stops
 * after the lease is applied. Anywhere else this IS the finishing: net_init
 * ran long ago, so a lease that changes the address has to announce it
 * itself (RFC 5227 2.3), and the log has to say a lease arrived late.
 * Sockets are safe: apply_lease() publishes under the network lock, and the
 * routing table is pulled from net_cfg on use (c/net/ip/ip.c route_sync). */
static void dhcp_accept_lease(const struct bootp_hdr *h,
                              const uint8_t *opts, int olen)
{
    int was_state = st;
    uint32_t previous_ip = net_cfg.ip;
    apply_lease(h, opts, olen);
    have_lease = 1;
    st = ST_BOUND;
    if (initial)
        return;
    if (was_state == ST_REQUESTING)
        dhcp_report_background_lease("background");
    else if (was_state == ST_INIT_REBOOT)
        dhcp_report_background_lease("background, INIT-REBOOT");
    if (net_cfg.ip != previous_ip && LOGIT_HAVE(arp_announce))
        arp_announce();
}

static void dhcp_handle(const uint8_t *pkt, int len)
{
    if (len < (int)sizeof(struct bootp_hdr))
        return;
    const struct bootp_hdr *h = (const struct bootp_hdr *)pkt;
    /* RFC 2131 4.1: a reply must echo our xid and chaddr, carry the magic
     * cookie, and be a BOOTREPLY. */
    if (h->op != 2 || ntohl(h->magic) != DHCP_MAGIC || ntohl(h->xid) != xid)
        return;
    for (int i = 0; i < 6; i++)
        if (h->chaddr[i] != net_cfg.mac[i]) return;
    const uint8_t *opts = pkt + sizeof *h;
    int olen = len - (int)sizeof *h;
    const uint8_t *t = opt_find(opts, olen, 53, 1);
    if (!t) return;

    if (*t == DHCP_OFFER && st == ST_SELECTING) {
        offered = ntohl(h->yiaddr);
        const uint8_t *v = opt_find(opts, olen, 54, 4);
        server_id = v ? get32n(v) : ntohl(h->siaddr);
        send_request();
        enter(ST_REQUESTING);
    } else if (*t == DHCP_ACK &&
               (st == ST_REQUESTING || st == ST_RENEWING || st == ST_INIT_REBOOT)) {
        dhcp_accept_lease(h, opts, olen);
    } else if (*t == DHCP_NAK && st != ST_BOUND && st != ST_IDLE) {
        /* The server refused what we asked for: start over. RFC 2131 3.2: a
         * NAK to INIT-REBOOT means the remembered address is not valid on
         * this network, so the lease is forgotten and the next link-up asks
         * from scratch. */
        have_lease = 0;
        send_discover();
        enter(ST_SELECTING);
    }
}

/* Start the acquisition again because the link has just come back. A client
 * that holds a lease asks to keep it (INIT-REBOOT: one REQUEST, RFC 2131 3.2
 * and 4.3.2) -- the common case is a cable bump or a router reboot, and the
 * same address back keeps every open connection alive. A client without one
 * broadcasts DISCOVER. Both go out NOW, not after a backoff: a link that was
 * down is the one moment waiting longer cannot help. */
static void dhcp_restart_on_link(uint64_t now_ns)
{
    acquire_start_ns = now_ns;
    dhcp_new_xid();
    if (have_lease && net_cfg.ip) {
        enter(ST_INIT_REBOOT);
        send_request();
        kprintf("[dhcp] link up; INIT-REBOOT for " DHCP_IP_FMT "\n", DHCP_IP_ARGS(net_cfg.ip));
        return;
    }
    send_discover();
    enter(ST_SELECTING);
    kprintf("[dhcp] link up; DISCOVER\n");
}

/* Follow the link between polls. Returns whether the wire is usable now. A
 * down edge pauses the client (nothing can be sent into a dead link, and the
 * lease is kept: RFC 2131 gives a client no reason to drop it); an up edge
 * restarts the acquisition. net_cfg is untouched either way. */
static int dhcp_link_follow(uint64_t now_ns)
{
    int link = dhcp_link_up();
    if (link == link_was_up)
        return link;
    link_was_up = link;
    if (!link) {
        kprintf("[dhcp] link down; client paused%s\n", have_lease ? ", lease kept" : "");
        return 0;
    }
    dhcp_restart_on_link(now_ns);
    return 1;
}

/* One background retransmission decision (RFC 2131 4.1). SELECTING repeats the
 * DISCOVER for as long as it takes, at 4, 8, 16, 32, then 64 s. REQUESTING and
 * INIT-REBOOT give their server DHCP_REQUEST_TRIES messages and then start
 * over from DISCOVER with a fresh xid -- silence from the server that offered
 * (or that held the lease) is not a reason to wait on it forever. In the
 * INIT-REBOOT case the remembered address stays configured until a new lease
 * replaces it (RFC 2131 3.2: the client may keep using it for the unexpired
 * lease). Nothing is sent while the link is down. */
static void dhcp_backoff_step(uint64_t now_ns, int link)
{
    if (!link || now_ns < next_send_ns)
        return;
    int server_silent = (st == ST_REQUESTING || st == ST_INIT_REBOOT) &&
                        retries + 1 >= DHCP_REQUEST_TRIES;
    if (server_silent) {
        dhcp_new_xid();
        send_discover();
        enter(ST_SELECTING);
        return;
    }
    if (st == ST_SELECTING)
        send_discover();
    else
        send_request();
    retries++;
    next_send_ns = now_ns + dhcp_backoff_ns((unsigned)retries);
}

/* One pump of the client: drain the socket, retransmit on silence, and move
 * BOUND -> RENEWING at T1. After the boot attempt it also follows the link and
 * paces its retransmits by the RFC 2131 backoff instead of a flat second. */
static void dhcp_step(void)
{
    IO_DOMAIN_GUARD(&dhcp_owner);
    if (st == ST_IDLE || sock < 0)
        return;
    uint8_t pkt[1500];
    int n;
    while ((n = udp_recv(sock, pkt, sizeof pkt, NULL, NULL)) > 0)
        dhcp_handle(pkt, n);

    uint64_t now = timer_ticks();
    uint64_t now_ns = time_mono_ns();
    /* The boot attempt owns the link wait itself; only the client that
     * outlives it follows edges. */
    int link = initial ? 1 : dhcp_link_follow(now_ns);
    if (st == ST_BOUND) {
        if (link && now >= renew_at) {
            enter(ST_RENEWING);     /* first, so build_msg sets ciaddr */
            send_request();         /* ciaddr renewal (RFC 2131 4.4.5) */
        }
        return;
    }
    if (!initial && st != ST_RENEWING) {
        dhcp_backoff_step(now_ns, link);
        return;
    }
    if (!link || now - st_tick < DHCP_RETRY_TICKS)
        return;
    if (++retries > DHCP_MAX_RETRIES) {
        if (initial) {
            st = ST_IDLE;           /* lets dhcp_run() report the failure */
        } else {
            /* Renewal keeps trying forever -- a dead server must not take
             * down a working config. After NAK/silence we re-DISCOVER, and
             * from there the backoff above paces it. */
            send_discover();
            enter(ST_SELECTING);
        }
        return;
    }
    if (st == ST_SELECTING) send_discover();
    else                    send_request();
    st_tick = now;
}

/* Hand the failed boot attempt over to the background client instead of
 * stopping. Caller holds dhcp_owner. The xid is kept: an OFFER that answers
 * one of the boot attempt's DISCOVERs and merely arrives late is still a valid
 * reply (RFC 2131 4.4.1). The last boot DISCOVER went out under a second ago,
 * so the first background retransmit is one backoff step away; if the link is
 * down nothing is due until it comes up and dhcp_link_follow() restarts. */
static void dhcp_background_begin(int link)
{
    initial = 0;
#ifdef DHCP_NEGCTL_GIVE_UP_AFTER_BOOT
    /* NEGATIVE CONTROL: the old behaviour, one boot attempt and then nothing.
     * The background-lease host gates MUST fail against this build. */
    (void)link;
    st = ST_IDLE;
#else
    link_was_up = link;
    enter(ST_SELECTING);
    kprintf("[dhcp] background client keeps trying (RFC 2131 4.1 backoff 4..64 s, restarts on link up)\n");
#endif
}

/* Wait for the PHY at boot. The link flag only moves when the NIC driver is
 * polled (the rtl8169 reads its PHY status in its RX poll), so the wait IS a
 * net_poll() loop. Bounded by DHCP_LINK_WAIT_NS of TSC time. Returns 1 when
 * the link is up. A DISCOVER sent into a link that is still negotiating is
 * simply lost, and with only three retransmits that was the whole boot
 * attempt on the X79-VG4. */
static int dhcp_wait_for_link(void)
{
    uint64_t t0 = time_mono_ns();
    uint64_t next = t0 + 1000000000ull;
    int waited = 0;
    for (;;) {
        net_poll();
        uint64_t now = time_mono_ns();
        if (dhcp_link_up()) {
            if (waited)
                kprintf("[dhcp] link up after %lu ms\n", (unsigned long)((now - t0) / 1000000ull));
            return 1;
        }
        waited = 1;
        if (now - t0 >= DHCP_LINK_WAIT_NS)
            return 0;
        if (now >= next) {
            kprintf("[dhcp] waiting for link %lus\n", (unsigned long)((now - t0) / 1000000000ull));
            next += 1000000000ull;
        }
        dhcp_wait();
    }
}

int dhcp_run(int timeout_ticks)
{
    unsigned zero = 0;
    if (!__atomic_compare_exchange_n(&dhcp_runner, &zero, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return -1;
    unsigned claim __attribute__((cleanup(dhcp_runner_drop))) = 1;
    {
    IO_DOMAIN_GUARD(&dhcp_owner);
    if (sock < 0) {
        sock = udp_bind(DHCP_SPORT);
        if (sock < 0)
            return -1;
    }
    acquire_start_ns = time_mono_ns();
    }

    if (!dhcp_wait_for_link()) {
        kprintf("[dhcp] link still down after %lu ms; boot continues\n",
                (unsigned long)((time_mono_ns() - acquire_start_ns) / 1000000ull));
        IO_DOMAIN_GUARD(&dhcp_owner);
        dhcp_new_xid();
        dhcp_background_begin(0);
        return -1;
    }
    {
    IO_DOMAIN_GUARD(&dhcp_owner);
    dhcp_new_xid();
    link_was_up = 1;
    initial = 1;
    send_discover();
    enter(ST_SELECTING);
    }

    /* Bounded twice: by PIT ticks as before, and by the TSC wall clock,
     * which needs no interrupt -- on a machine whose timer interrupt stopped,
     * the tick bound alone never expires. One progress line per second says
     * which clock is moving (X79-VG4 bring-up, 2026-10-31). */
    uint64_t start = timer_ticks();
    uint64_t t0 = time_mono_ns(), next = t0 + 1000000000ull;
    uint64_t wall = (uint64_t)timeout_ticks * 10000000ull + 1000000000ull;
    while (timer_ticks() - start < (uint64_t)timeout_ticks && time_mono_ns() - t0 < wall) {
        net_poll();
        dhcp_step();
        { IO_DOMAIN_GUARD(&dhcp_owner);
          if (st == ST_BOUND) { initial = 0; return 0; } }
        uint64_t now = time_mono_ns();
        if (now >= next) {
            kprintf("[dhcp] waiting %lus: pit ticks %lu, nic irqs %lu\n",
                    (unsigned long)((now - t0) / 1000000000ull),
                    (unsigned long)(timer_ticks() - start),
                    (unsigned long)(LOGIT_HAVE(&rtl8169_irq_count) ? rtl8169_irq_count : 0));
            next += 1000000000ull;
        }
        dhcp_wait();
    }
    kprintf("[dhcp] no lease after %lu ms (pit ticks %lu)\n",
            (unsigned long)((time_mono_ns() - t0) / 1000000ull), (unsigned long)(timer_ticks() - start));
    { IO_DOMAIN_GUARD(&dhcp_owner); dhcp_background_begin(dhcp_link_up()); }
    return -1;
}

void dhcp_poll(void)
{
    dhcp_step();
}

void dhcp_status(struct dhcp_status *out)
{
    /* A diagnostic snapshot taken without dhcp_owner: the HUD must not sleep
     * on a mutex the client holds across a transmit, and a torn read of a
     * counter here costs one wrong digit on a screen that refreshes in a
     * second. */
    static const char *const names[] = {
        "idle", "selecting", "requesting", "bound", "renewing", "init-reboot"
    };
    int state = st;
    out->state = (state >= 0 && state < (int)(sizeof names / sizeof names[0])) ? names[state] : "?";
    out->retries = retries > 0 ? (unsigned)retries : 0;
    out->link_up = link_was_up;
    out->next_seconds = 0;
    if (state == ST_BOUND) {
        uint64_t now = timer_ticks();
        out->next_seconds = renew_at > now ? (unsigned)((renew_at - now) / 100) : 0;
    } else if (state != ST_IDLE && !initial && link_was_up) {
        uint64_t now_ns = time_mono_ns();
        out->next_seconds = next_send_ns > now_ns ? (unsigned)((next_send_ns - now_ns) / 1000000000ull) : 0;
    }
}
