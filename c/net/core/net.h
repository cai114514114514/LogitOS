/* 2026-09-10 concurrency correction: DNS, DHCP, socket, HTTP and TLS state now have distinct task owners; ICMP uses net_lock and route.c a short table gate. Their old BKL inventory below is historical. No service owner spans the global network polling loop. */
#ifndef LOGIT_NET_H
#define LOGIT_NET_H

#include <stdint.h>

/* Host byte order helpers (x86 is little-endian; network is big-endian). */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x)
{
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Build an IPv4 address (a.b.c.d) in host order. */
#define IPV4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                          ((uint32_t)(c) << 8) | (uint32_t)(d))

struct net_config {
    uint8_t  mac[6];
    uint32_t ip;        /* host order */
    uint32_t mask;
    uint32_t gw;
    uint32_t dns;       /* resolver (DHCP option 6, or static fallback) */
};

extern struct net_config net_cfg;

/* Bring up the NIC + stack (called from kmain). Returns 0 on success. */
int  net_init(void);
int  net_up(void);                  /* 1 if a NIC initialised successfully */

/* Pump the receive path (called from the WM main loop). */
void net_poll(void);

/* Called by a NIC ISR once it has acked its device: the drain is handed to
 * SOFTIRQ_NET rather than run inside the interrupt. Safe from interrupt
 * context; see the receive-path comment in net.c for the whole story. */
void net_rx_schedule(void);
void net_idle(void);                /* sti;hlt -- yield the host CPU while a blocking fetch waits */
extern volatile int g_net_busy;     /* 1 while a blocking fetch owns the network */

/* TCP's timer pass no longer rides the window manager's loop: a 10 ms ktimer
 * raises SOFTIRQ_NET and the pass runs there. See the long block above
 * net_init() in net.c for the locking argument and the cadence. These three are
 * the observability and the negative control; `fires` counts ktimer callbacks,
 * `softirq` passes that ran on the softirq, `inl` passes net_poll() had to
 * discharge because the raise landed on a nested entry. */
void net_tcp_timer_stats(uint32_t *fires, uint32_t *softirq, uint32_t *inl);
/* TEST ONLY, both of them, reached from `echo netwedge <ms> [wm|ktimer] >
 * /dev/ktrigger`. The first parks net_poll() for <ms> -- a wedged compositor as
 * far as the network can tell. The second forces the pre-change wiring back on
 * at runtime, which is the negative control the wedge test must fail against. */
void net_debug_park(long ms);
void net_debug_tcp_on_wm(int on);

/* ---------------------------------------------------------------------------
 * MUTUAL EXCLUSION FOR THE NETWORK STACK. Recursive, IRQ-off, cross-core.
 *
 * THIS USED TO BE A BARE `cli`, and the comment that stood here said so:
 * "Everything net runs on the BSP, and the NIC IRQ is routed to the BSP, so
 * masking interrupts is enough." The second clause is true by routing. The
 * first was true only because of the big kernel lock -- tcp_send/tcp_recv/
 * tcp_connect/udp_send/arp_resolve are reached from SYSCALLS, which run on
 * whatever core made them. `cli` on core 0 says nothing about core 1.
 *
 * THE SCOPE, stated once, because a lock without a stated scope is a lock
 * nobody can reason about. Held, it gives exclusive access to:
 *
 *   c/net/transport/tcp.c   conns[], listeners[] (their FIELDS; the backlog
 *                           queue q[]/qn is l->wq.lock's), next_port,
 *                           iss_counter, the five st_* server counters
 *
 *   ...and the ONE PLACE THOSE TWO LOCKS MEET, because the split above was the
 *   sentence and the code disagreed with it. tcp_accept() and
 *   tcp_listen_close() reach BOTH sets, and both used to take only lq->lock.
 *   Both now take net_lock OUTSIDE lq->lock -- the order backlog_push() already
 *   uses, never the reverse -- and the reason is the same in both: a backlog
 *   entry is a BARE CONNECTION INDEX, and an index only means anything while
 *   the lock that governs the slot table is held. Drop it between publishing a
 *   slot as free-able and acting on the index, and a net_lock holder on another
 *   core can free the slot and hand it to a new connection in between. See the
 *   two comments at those functions for the exact interleavings; both end in a
 *   live connection being reset or dropped with no counter moved.
 *
 *   DELIBERATELY OUTSIDE, and named so the next reader does not have to
 *   rediscover that it was considered: tcp_alive(), tcp_connect_status() and
 *   tcp_wait_writable()'s predicate read conn fields with nothing held. Each is
 *   a single aligned scalar and each answer is a SNAPSHOT its caller re-tests;
 *   the worst case is one more pass round a poll loop, never a wrong action on
 *   a stale slot. That is a different class from the two above, which WRITE.
 *   c/net/transport/udp.c   socks[], unreach_ip, unreach_tick
 *   c/net/core/raw.c        socks[]
 *   c/net/link/arp.c        cache[], pendq[], stats
 *   c/net/link/eth.c        detag[], stats, loopback_depth -- BOTH directions
 *                           now: the RX drain already held it, and eth_send()
 *                           takes it so the loopback re-entry and every NIC's
 *                           TX ring are covered too (e1000.c:333 states that
 *                           contract; the UDP broadcast path broke it)
 *   c/net/ip/reasm.c        slots[]
 *   c/net/ip/ip6.c          the interface-address table, via ip6_poll
 *   the four NIC drivers    the RX descriptor ring, rx_cur, tx_cur, and the
 *   (c/drivers/net, virtio) read-to-clear NIC statistics registers
 *
 * WHAT IT DELIBERATELY DOES NOT COVER, still BKL-guarded: dns.c's dq[] and
 * dcache[], icmp.c's ping_* slot, route.c's tab[], dhcp.c's state machine,
 * sock.c/lsock.c's socks[], http.c's raw[], hpack's tables. Those are reached
 * from net_poll() and from syscalls but never from the RX interrupt, so `cli`
 * never covered them either; widening to them is a separate change with its
 * own argument (sock_pump() reaches tls_step(), and holding a network lock
 * across an RSA verify is not a trade).
 *
 * RECURSIVE ON PURPOSE: the receive path is ENTERED with the lock held by the
 * driver drain and re-takes it three levels down (eth_input -> arp_input; and
 * tcp_output -> ip_output -> arp_output -> arp_resolve). See the block at the
 * top of c/net/core/netlock.c for the derivation and the lock order.
 *
 * Host-side unit tests compile with -DLOGIT_NET_HOST: cli/sti are ring-0 only
 * and there is one thread, so the lock degenerates to a no-op there -- which
 * also means NO HOST GATE IN THIS TREE CAN OBSERVE THIS LOCK. Only a device
 * boot can, and `make test-netlock` is the one that does (with
 * test-netlock-negctl, an ISO built NETNOTXLOCK=1, as its prerequisite so the
 * control cannot be stranded). Measured 2026-08-28:
 *
 *   shipped      [netlock] acq 1758 recursive 5 maxdepth 2 violations 0
 *   NETNOTXLOCK  [netlock] acq 1316 recursive 0 maxdepth 1 violations 3
 *                + [netlock] BUG: entered without net_lock at eth_send
 *
 * Both boot, both take a lease, both fetch; nothing a person can see separates
 * them. Note the control also loses the NESTING -- recursive 0 / maxdepth 1 --
 * which is the design's own prediction falling out of the measurement: eth_send
 * is the funnel that re-enters the lock, so removing its acquisition removes
 * the recursion as well as the coverage.
 *
 * SECOND DOOR, unfixed and named: tests/unit/tcpstub/net.h carries its OWN
 * no-op copy of net_lock/net_unlock and does not declare net_lock_held,
 * net_lock_assert_held or net_lock_stats. It compiles today because tcp.c uses
 * none of those. A file that starts asserting will fail to build against the
 * stub and not against this header, which is the "one jar, two doors" shape --
 * recorded here rather than left to be re-derived at the error message.
 * ------------------------------------------------------------------------- */
#ifdef LOGIT_NET_HOST
static inline uint64_t net_lock(void) { return 0; }
static inline void net_unlock(uint64_t f) { (void)f; }
static inline int  net_lock_held(void) { return 1; }
static inline void net_lock_assert_held(const char *who) { (void)who; }
static inline void net_lock_stats(unsigned long *a, unsigned long *r,
                                  unsigned long *m, unsigned long *v)
{ if (a) *a = 0; if (r) *r = 0; if (m) *m = 0; if (v) *v = 0; }
#else
uint64_t net_lock(void);
void     net_unlock(uint64_t f);
int      net_lock_held(void);
/* Print-once-and-count if the caller does NOT hold it; `who` names the site.
 * This is what turns eth.h's "CONTRACT: callers hold net_lock()" from a
 * sentence into a check. */
void     net_lock_assert_held(const char *who);
/* acq = outermost acquisitions, recur = recursive re-entries, maxdepth =
 * deepest nesting seen, viol = assertion failures + unbalanced unlocks. */
void     net_lock_stats(unsigned long *acq, unsigned long *recur,
                        unsigned long *maxdepth, unsigned long *viol);
#endif

/* Scope the existing recursive protocol lock through every early return. */
struct net_guard { uint64_t flags; };
static inline void net_guard_drop(struct net_guard *g) { net_unlock(g->flags); }
#define NET_GUARD struct net_guard net_guard_ \
    __attribute__((cleanup(net_guard_drop))) = { net_lock() }
/* Service owners are not the packet lock. Take a small snapshot before
 * waiting or entering DNS/TLS; no network spinlock spans those operations. */
static inline struct net_config net_config_snapshot(void)
{ NET_GUARD; return net_cfg; }
#endif /* LOGIT_NET_H */
