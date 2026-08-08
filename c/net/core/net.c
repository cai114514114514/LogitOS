#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "netdev.h"
#include "eth.h"
#include "work.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

/* net_cfg starts empty; net_init fills it via DHCP, or with the static SLIRP
 * fallback below if negotiation fails. */
struct net_config net_cfg;

/* Static config matching QEMU SLIRP (user networking) defaults, used when
 * DHCP does not answer (and identical to what SLIRP hands out when it does). */
static void net_cfg_fallback(void)
{
    net_cfg.ip   = IPV4(10, 0, 2, 15);
    net_cfg.mask = IPV4(255, 255, 255, 0);
    net_cfg.gw   = IPV4(10, 0, 2, 2);
    net_cfg.dns  = IPV4(10, 0, 2, 3);
    kprintf("[net] dhcp failed; static ip 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3\n");
}

static int up;

int net_up(void) { return up; }

/* dhcp.c is linked into the kernel but kept optional like the other layers. */
int  dhcp_run(int timeout_ticks) __attribute__((weak));
void dhcp_poll(void) __attribute__((weak));

/* The settings store, weak for exactly the reason dhcp_run is: net.c is linked
 * into host tests that have no filesystem to read a settings file from. On the
 * machine these resolve; host-side they are NULL and the configuration is the
 * DHCP-then-static path this file always had. */
int      settings_get_int(const char *key, int def) __attribute__((weak));
unsigned settings_get_ip(const char *key, unsigned def) __attribute__((weak));

/* A user who has turned automatic configuration off. The addresses come from
 * the store and are already validated -- settings_get_ip() returns the schema
 * default for anything that is not a dotted quad, so `net.ip = 999.1.1.1` in a
 * hand-edited file configures 10.0.2.15 and does not configure garbage.
 *
 * Returns 1 if a static configuration was applied, 0 to fall through to DHCP. */
static int net_cfg_from_settings(void)
{
    if (!settings_get_int || !settings_get_ip) return 0;
    if (settings_get_int("net.dhcp", 1)) return 0;      /* automatic: DHCP below */

    net_cfg.ip   = settings_get_ip("net.ip",   IPV4(10, 0, 2, 15));
    net_cfg.mask = settings_get_ip("net.mask", IPV4(255, 255, 255, 0));
    net_cfg.gw   = settings_get_ip("net.gw",   IPV4(10, 0, 2, 2));
    net_cfg.dns  = settings_get_ip("net.dns",  IPV4(10, 0, 2, 3));
    kprintf("[net] static from settings: ip %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u\n",
            (net_cfg.ip >> 24) & 255, (net_cfg.ip >> 16) & 255,
            (net_cfg.ip >> 8) & 255, net_cfg.ip & 255,
            (net_cfg.gw >> 24) & 255, (net_cfg.gw >> 16) & 255,
            (net_cfg.gw >> 8) & 255, net_cfg.gw & 255,
            (net_cfg.dns >> 24) & 255, (net_cfg.dns >> 16) & 255,
            (net_cfg.dns >> 8) & 255, net_cfg.dns & 255);
    return 1;
}

/* Link-layer hooks, weak for the same reason: net.c is linked into host tests
 * that have no NIC and no ARP. */
void arp_announce(void) __attribute__((weak));
void arp_dump(void) __attribute__((weak));
void eth_dump(void) __attribute__((weak));

/* ---------------------------------------------------------------------------
 * The receive path.
 *
 * A NIC ISR used to drain the whole ring itself: eth_input -> ip_input ->
 * tcp_input, for up to a ring's worth of frames, INSIDE the interrupt and
 * BEFORE the EOI. On a 900 KiB fetch that is ~630 frames of protocol work done
 * at the highest priority in the machine, with the NIC's vector held the whole
 * time.
 *
 * Now the ISR does only what an ISR must: ack the device, and say that a drain
 * is owed. The drain runs on SOFTIRQ_NET, which interrupts.c runs at the tail
 * of the same interrupt -- after the EOI, so the NIC is free to re-assert while
 * we work, and coalesced, because softirq_raise() is idempotent and several
 * arrivals collapse into one drain.
 *
 * WHY NOT THE WORKQUEUE TIER, which is the one that may sleep: nothing in
 * eth/ip/tcp/udp input blocks -- they take net_lock (cli) and return -- so a
 * thread hand-off would buy nothing and cost a context switch per burst, plus
 * the receive latency of getting kworker scheduled. The tier is chosen by what
 * the work needs, and this work needs "soon and bounded", not "may wait".
 *
 * THE ONE CASE THE SOFTIRQ CANNOT COVER, and why the inline fallback exists.
 * interrupts.c runs softirqs only when the entry OWNS the BKL (`!nested`). A
 * NIC interrupt that lands inside a kernel `sti` window -- SYS_HTTP_GET runs
 * with IF=1 on purpose, see the M11 note -- is nested, so its tail does not run
 * softirqs and the raise sits pending until the outer entry finishes. The ring
 * is 64 descriptors, about 3 ms of a saturated receive, so "pending until the
 * syscall returns" would be an overflow, not a delay. So: if a drain is still
 * owed when the next interrupt arrives, the previous raise demonstrably did not
 * run, and this one drains inline instead of queueing a second promise behind
 * the first.
 * ------------------------------------------------------------------------- */
static volatile uint32_t rx_owed;      /* a raise is outstanding */
static uint32_t rx_n_softirq, rx_n_inline, rx_n_poll, rx_n_irq, rx_frames;
static uint32_t idle_halts, idle_skips;   /* net_idle: how often it slept vs looked again */
/* 64, not 512: a fetch has to be reported while it is still small enough to
 * have IDLE GAPS in it, because "did an interrupt wake us" is only a question
 * when the machine ever got to sleep. A boot with no traffic receives well
 * under this (DHCP is two inbound frames plus ARP), so the line stays out of
 * every other line's serial expectations. */
static uint32_t rx_next_report = 64;

/* Counts drains that actually DELIVERED something. net_poll runs ~100x/s and
 * mostly finds an empty ring; counting those would drown the comparison this
 * exists to make ("which context is the receive path?") in idle polls. */
static void net_rx_drain(uint32_t *counter)
{
    rx_owed = 0;                       /* whoever drains discharges the debt */
    int n = netdev_rx_poll(eth_input);
    if (n > 0) { (*counter)++; rx_frames += (uint32_t)n; }
}

/* SOFTIRQ_NET handler: the receive path proper. */
static void net_rx_softirq(void)
{
    net_rx_drain(&rx_n_softirq);
}

/* Called by every NIC ISR once it has acked its device. Interrupt context. */
void net_rx_schedule(void)
{
    if (!up) return;
    rx_n_irq++;
    if (rx_owed) {                     /* the last raise never ran -- see above */
        net_rx_drain(&rx_n_inline);
        return;
    }
    rx_owed = 1;
    softirq_raise(SOFTIRQ_NET);
}

/* One line, only under real traffic. A desktop boot receives a couple of dozen
 * frames (DHCP, ARP), so this stays silent unless something actually moved
 * bytes -- which keeps it out of every other line's serial expectations while
 * still being the thing tests/boot/run-net-rx-test.sh reads. */
static void rx_report(void)
{
    if (rx_frames < rx_next_report) return;
    /* Re-report every 512 frames, so a long fetch ends with a line that
     * describes the WHOLE transfer rather than only its first moments -- the
     * counters at frame 64 are a snapshot of the handshake, not of the path. */
    rx_next_report = rx_frames + 512;
    kprintf("[net] rx path: frames %u irq %u softirq %u inline %u poll %u "
            "idle %u/%u halt/skip\n",
            rx_frames, rx_n_irq, rx_n_softirq, rx_n_inline, rx_n_poll,
            idle_halts, idle_skips);
}

int net_init(void)
{
    if (netdev_init() != 0)
        return -1;
    memcpy(net_cfg.mac, netdev_mac(), 6);
    up = 1;
    softirq_register(SOFTIRQ_NET, net_rx_softirq);
    netdev_irq_enable(eth_input);   /* IRQ-driven RX; net_poll still backstops */
    /* Unconfigured while negotiating: DHCPDISCOVER leaves from 0.0.0.0, and
     * only broadcast UDP can come back in until we own an address. */
    if (!net_cfg_from_settings())
        if (!dhcp_run || dhcp_run(300) != 0)    /* ~3 s */
            net_cfg_fallback();

    /* RFC 5227 s3: announce the address we just took. Until now the machine
     * never announced at all -- nothing on the segment learned our binding
     * until we happened to talk to it, switches did not learn our port until we
     * transmitted, and a second host taking the same address was never noticed
     * by anybody. One broadcast fixes all three, and it is also what makes the
     * conflict counter in arp_dump() able to be non-zero. */
    if (arp_announce) arp_announce();

    /* One line each from the two link-layer files, once, at the end of bring-up.
     * They are the only visibility this layer has ever had; a machine dropping
     * every frame for a wrong-destination or unsupported-ethertype reason used
     * to look exactly like a machine on an idle network. */
    if (eth_dump) eth_dump();
    if (arp_dump) arp_dump();
    return 0;
}

void tcp_poll(void) __attribute__((weak));
void ip_poll(void) __attribute__((weak));
/* The IPv4 neighbour cache's clock: solicitation retransmits, REACHABLE->STALE
 * ageing, unicast probes, expiry of failed lookups. Without a periodic call
 * nothing in c/net/link/arp.c ever ages, which is the state the old flat cache
 * was permanently in. */
void arp_poll(void) __attribute__((weak));
void dns_poll(void) __attribute__((weak));   /* async resolver pool */
void sock_pump(void) __attribute__((weak));  /* non-blocking client sockets */

/* Set while a blocking fetch (http_get/res_fetch, on the app thread) owns the
 * network. The WM render thread is preempted into ~100x/s and also calls
 * net_poll; without this, two threads would drive e1000 RX + mutate conns[]/the
 * DNS recv slot concurrently, corrupting in-flight handshakes (intermittent
 * rc=-3/-5). The fetch polls itself; the WM skips polling while we're busy. */
volatile int g_net_busy = 0;

void net_poll(void)
{
    if (up) {
        /* Still the backstop, and still first: a polled-only card has no other
         * receive path at all, and even an IRQ-driven one can have a raise
         * pending from a nested entry (see net_rx_schedule). */
        net_rx_drain(&rx_n_poll);
        rx_report();
        if (tcp_poll) tcp_poll();
        if (ip_poll) ip_poll();
        if (arp_poll) arp_poll();
        if (dhcp_poll) dhcp_poll();
        /* The async half, and the reason a fetch no longer freezes the desktop:
         * these two advance EVERY outstanding lookup and EVERY open socket by
         * whatever the bytes already received allow, then return. Ordering is
         * deliberate -- dns_poll first, so a name that resolves on this pass has
         * its socket move on to the SYN in the same pass rather than the next. */
        if (dns_poll) dns_poll();
        if (sock_pump) sock_pump();
    }
}

/* Idle the CPU until the next interrupt, between net_poll()s in the blocking
 * fetch loops (dns/tcp/tls). They used to busy-spin, which under QEMU TCG pegs
 * the *host* CPU and makes the whole desktop crawl during a page load. `sti;hlt`
 * halts until an interrupt wakes us -- the NIC RX IRQ (or, failing that, the
 * timer) -- so the loop re-polls promptly without burning host cycles.
 * (SYS_HTTP_GET already runs with IF=1, so the hlt can actually wake.)
 *
 * DO NOT SLEEP ON WORK THAT HAS ALREADY ARRIVED. Every blocking caller is a
 * loop of the shape "pump the network, test a condition, wait" -- and the TLS
 * one tests BEFORE it pumps:
 *
 *     for (;;) { rc = tls_step(id); if done -> return; net_poll(); net_idle(); }
 *
 * so the bytes net_poll() just delivered are bytes tls_step has not seen. An
 * unconditional halt there parks the machine until some unrelated interrupt
 * happens along, and adds up to a whole wake period to every round trip --
 * measured from the guest, a wake period of 9.87 ms mean before the NIC
 * interrupt was made to fire at all (it is the 100 Hz tick, exactly) and
 * 1.99 ms after. Against a peer on the same host, whose replies come back in
 * a third of a millisecond, that is the entire latency.
 *
 * So: if the receive path has delivered anything since the last time we were
 * here, return without halting and let the caller look again. This cannot
 * become a spin -- a second skip requires a second frame -- and it needs no
 * cooperation from the callers, which is the point: tls.c, dns.c, arp.c and
 * tcp.c belong to other lines.
 *
 * THE SKIP DROPS THE `hlt` AND NOTHING ELSE -- it still executes the `sti`, and
 * that is deliberate rather than tidy. int 0x80 is an interrupt gate, so a
 * syscall body runs with IF=0 unless something sets it; SYS_HTTP_GET sets it
 * once, and every net_lock()/net_unlock() pair below it only RESTORES whatever
 * IF was on entry. The old unconditional `sti; hlt` therefore re-armed
 * interrupts on every trip round every blocking loop, and nothing in the tree
 * says that was incidental. A skip that returned early would quietly remove
 * that re-arm from paths nobody has audited, which is not a thing to do while
 * changing something else.
 */
void net_idle(void)
{
    static uint32_t seen;
    if (rx_frames != seen) {        /* the network answered; go look at it */
        seen = rx_frames;
        __asm__ volatile ("sti");
        return;
    }
    __asm__ volatile ("sti\n\thlt");
    /* Deliberately NOT resyncing `seen` here: frames delivered by the interrupt
     * that just woke us have not reached the caller's condition yet, so the
     * next call must also decline to halt and give the caller one more look. */
}
