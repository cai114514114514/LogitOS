#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "netdev.h"
#include "eth.h"
#include "work.h"
#include "ktime.h"
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

/* Defined with the TCP timer block below. SOFTIRQ_NET now has two producers --
 * the NIC ISR (a drain is owed) and a 10 ms ktimer (a TCP timer pass is owed) --
 * and each discharges only its own debt, so a raise from one never eats the
 * other's work. Two producers on one softirq is safe for the reason work.h
 * gives: softirq_raise() is idempotent and the handler runs on the raising
 * core. */
static void tcp_tick_softirq_run(void);

/* SOFTIRQ_NET handler: the receive path, and TCP's timers.
 *
 * ORDER IS DELIBERATE. Drain first: a segment that arrived in this interrupt
 * may be the ACK that cancels the retransmit the timer pass is about to send,
 * or the window update that lets the drain push bytes. Running the timer first
 * would retransmit data the peer had already acknowledged one line earlier. */
static void net_rx_softirq(void)
{
    net_rx_drain(&rx_n_softirq);
    tcp_tick_softirq_run();
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

/* ---------------------------------------------------------------------------
 * TCP'S CLOCK, AND WHY IT IS NOT THE COMPOSITOR'S ANY MORE.
 *
 * tcp_poll() is not a poll. It is TCP's timer wheel: the retransmission
 * timeout, the delayed-ACK flush, the zero-window persist probe, the
 * FIN_WAIT/TIME_WAIT reaping that returns connection slots, and the drain that
 * pushes queued bytes once the peer's window reopens. Every one of those is a
 * DEADLINE, and until now the only steady-state caller was
 *
 *     c/kernel/gui/wm.c   if (!g_net_busy) net_poll();   <- THE one line
 *
 * i.e. once per composited frame. So every deadline in tcp.c ran at the
 * compositor's frame rate, and on a machine whose compositor was wedged it did
 * not run at all -- include/abi/logit_abi.h says it in its own words: such a
 * machine "accepts connections and answers them slowly or not at all". The
 * receive path was moved off that loop already (SOFTIRQ_NET, above); this is
 * the other half.
 *
 * BE PRECISE ABOUT THE CLIENT PATH, because the obvious wording is wrong. A
 * blocking fetch sets g_net_busy so the WM SKIPS its call -- but the fetch's
 * own loop calls net_poll() thousands of times a second, so during a fetch the
 * timers ran faster than ever. The gap is the case where NEITHER runs, and the
 * server path is exactly it: c/net/core/lsock.c's read and write park on
 * tcp_wait_readable/tcp_wait_writable and never call net_poll at all. Its own
 * comment already said half of this ("what wakes it is the segment ... so this
 * does not depend on the window manager running. (The RESPONSE going back out
 * does)"). This is the other half of that parenthesis.
 *
 * WHERE IT RUNS NOW, AND WHY IT IS THE SAME LOCK DISCIPLINE AS RX.
 * A periodic ktimer raises SOFTIRQ_NET; the pass runs in the softirq handler.
 * That is the identical context tcp_input() has run in since the RX move:
 * interrupt context, IF=0, WITH the big kernel lock held (interrupts.c runs
 * softirqs at `done:` only when `!nested`, i.e. only when the entry owns the
 * BKL). tcp.c's own mutual exclusion is net_lock(), which is `cli` -- so the
 * pass is protected exactly as it was when the WM thread called it: BKL against
 * the other cores, IF=0 against this core's NIC interrupt. Nothing in tcp_poll
 * is new to that context either: send_seg/tcp_output/conn_closed/
 * waitq_wake_all are all already reached from tcp_input on this same softirq.
 *
 * WHY NOT THE KTIMER CALLBACK DIRECTLY. ktime.h is explicit that a callback
 * runs "WITHOUT the big kernel lock", from the pre-BKL window in
 * interrupts.c -- deliberately, so a deadline can never queue behind a lock
 * held by a thread waiting for that deadline. Walking conns[] there would race
 * a tcp_send() running under the BKL on another core, which is a race that does
 * not exist today. So the callback does the one thing that is safe without any
 * lock at all: an atomic bit, which is what softirq_raise() is.
 *
 * WHY NOT THE WORKQUEUE (the tier that may sleep). Same answer work.h already
 * gives for the receive path: nothing in tcp_poll blocks -- it takes net_lock
 * and returns -- so a thread hand-off would buy nothing and cost a context
 * switch per tick. The tier is chosen by what the work needs.
 *
 * THE CADENCE IS 10 ms BECAUSE THAT IS THE CLOCK tcp.c READS. Every deadline in
 * tcp.c is compared against timer_ticks(), the 100 Hz PIT counter, so one tick
 * is the finest deadline the file can express and a faster timer could not
 * change a single decision. Against that clock the worst-case lateness this
 * adds is one tick: 10% of RTO_MIN (100 ms) and 5% of DELACK_MAX (200 ms). A
 * 100 ms period -- "the RTO granularity" -- was the other candidate and is
 * wrong: it quantises RTO_MIN to 0..100 ms of extra delay, doubling the
 * worst-case retransmit on a fast path, and pushes the delayed ACK toward RFC
 * 1122's 500 ms MUST-NOT for no saving worth having.
 *
 * WHAT net_poll() DOES WITH IT NOW. It discharges an OWED pass rather than
 * driving one, which is the same shape net_rx_schedule() uses for the raise it
 * cannot be sure ran. That matters for one real case: interrupts.c runs
 * softirqs only on a non-nested entry, and a timer interrupt landing inside a
 * kernel `sti` window (SYS_HTTP_GET runs with IF=1 on purpose) is nested, so
 * its raise sits pending. Every such window is a blocking fetch whose own loop
 * calls net_poll(), so the debt is discharged there instead -- in thread
 * context, under the BKL, exactly where tcp_poll() has always run.
 *
 * So that line in wm.c is now harmless and its owner may delete it whenever
 * they like: with the timer armed, that call finds an owed pass at most once
 * per tick and otherwise costs one load of a global and a branch. (Its line
 * NUMBER is deliberately not quoted here -- it was 5483 when this was written
 * and 5550 four hours later, because that file is being edited by another
 * line. The text is the anchor.)
 *
 * MEASURED ON THE MACHINE, 2026-08-21 (all TCG, -smp 4 -m 512M):
 *   - With net_poll parked and a 1.5 s wire cut mid-response, an inbound
 *     /bin/httpd fetch recovers in 1.60 s and delivers all 35,149 bytes, with
 *     the park reporting 325 ktimer fires and 325 softirq passes over the
 *     window. The same wedge with this switch flipped back reports 0 fires and
 *     does not recover in 20 s. tests/net.mk, `make test-tcp-timer`.
 *   - Throughput is unchanged: 301.4 -> 300.8 Mbit/s median over 9 paired reps
 *     of a 917,504-byte body, per-rep ratio median 1.004, e1000 RNBC/MPC both
 *     zero on both arms. The pass got RARER on the fetch path (net_poll used to
 *     call it on every trip round the blocking loop, ~490/s; it is 100/s now)
 *     and nothing measurable moved, which is what the tick-granularity argument
 *     above predicts.
 * ------------------------------------------------------------------------- */

void tcp_poll(void) __attribute__((weak));

/* One PIT tick. See the cadence argument above. */
#define TCP_TICK_NS  (10ull * NS_PER_MS)

static struct ktimer   tcp_tick_timer;
static volatile uint32_t tcp_tick_owed;     /* a pass is due and has not run */
static uint32_t tcp_tick_fires;             /* ktimer callbacks */
static uint32_t tcp_tick_softirq;           /* passes run on SOFTIRQ_NET */
static uint32_t tcp_tick_inline;            /* passes discharged by net_poll */

/* THE OLD WIRING, on a switch, because "the timers are off the WM loop" is a
 * claim that has to be watched failing. Set it and net_poll() drives tcp_poll()
 * unconditionally and the ktimer raises nothing -- byte for byte the behaviour
 * this file had before. -DTCP_TIMERS_ON_WM makes it the permanent build (the
 * negative control the design was written against); the runtime setter below is
 * how a device harness reaches it without a second kernel. */
#ifdef TCP_TIMERS_ON_WM
static int tcp_on_wm = 1;
#else
static int tcp_on_wm = 0;
#endif

/* Run an owed pass, if one is owed. Callable from the softirq tail or from
 * net_poll(); both hold the BKL, and tcp_poll() takes net_lock() itself. */
static void tcp_tick_run(uint32_t *counter)
{
    /* An EXCHANGE, not a test-then-clear, and for the same reason
     * softirq_run_pending() uses one on g_pending: there are two discharging
     * contexts (this core's softirq tail and a thread inside net_poll) and one
     * producer that is an interrupt. A read-modify-write with the raise landing
     * in the middle would drop that tick's pass -- bounded at 10 ms, never
     * systematic, and still not worth reasoning about when the atomic is free.
     * Claiming the debt before running it also means a raise DURING the pass is
     * kept, rather than being cleared by the pass that could not have seen it. */
    if (!__atomic_exchange_n(&tcp_tick_owed, 0u, __ATOMIC_SEQ_CST)) return;
    (*counter)++;
    if (tcp_poll) tcp_poll();
}

/* ktimer callback: INTERRUPT CONTEXT, NO BKL. One atomic OR and nothing else --
 * see the "why not the callback directly" paragraph above. */
static void tcp_tick_fire(struct ktimer *t)
{
    (void)t;
    if (tcp_on_wm) return;              /* the negative control: no raise at all */
    tcp_tick_fires++;
    __atomic_store_n(&tcp_tick_owed, 1u, __ATOMIC_SEQ_CST);
    softirq_raise(SOFTIRQ_NET);
}

static void tcp_tick_softirq_run(void) { tcp_tick_run(&tcp_tick_softirq); }

void net_tcp_timer_stats(uint32_t *fires, uint32_t *softirq, uint32_t *inl)
{
    if (fires)   *fires   = tcp_tick_fires;
    if (softirq) *softirq = tcp_tick_softirq;
    if (inl)     *inl     = tcp_tick_inline;
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

    /* TCP's clock, off the compositor. Armed AFTER the address is configured so
     * the first pass cannot run against a half-built stack, and last in this
     * function for the same reason.
     *
     * IF THE HEAP IS FULL WE DO NOT SILENTLY LOSE THE TIMERS. ktimer_add is the
     * only failure this can have (KTIMER_MAX is 128 and six are armed at boot),
     * and the honest answer to it is the old wiring plus a line saying so --
     * NOT a machine whose retransmits never run, which is what a swallowed -1
     * would build. */
    if (!tcp_on_wm) {
        if (ktimer_add(&tcp_tick_timer, TCP_TICK_NS, TCP_TICK_NS,
                       tcp_tick_fire, 0, "tcp") != 0) {
            tcp_on_wm = 1;
            kprintf("[net] tcp timers: ktimer heap FULL -- falling back to the "
                    "net_poll/WM wiring (retransmits run at the frame rate)\n");
        } else {
            kprintf("[net] tcp timers: 10 ms ktimer -> SOFTIRQ_NET "
                    "(independent of the WM loop)\n");
        }
    } else {
        kprintf("[net] tcp timers: ON THE WM LOOP (negative control build)\n");
    }
    return 0;
}

/* tcp_poll is declared with the timer block above, which is its driver now. */
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

/* ---------------------------------------------------------------------------
 * THE WM-WEDGE INSTRUMENT. Off unless armed; armed only from
 * `echo netwedge <ms> > /dev/ktrigger`.
 *
 * The property under test is "TCP's timers no longer depend on net_poll() being
 * called", and the only way to observe it is a machine on which net_poll() is
 * NOT being called while the network is expected to keep working. The WM loop's
 * entire contribution to the network is that one `if (!g_net_busy)
 * net_poll();` in wm.c, so parking
 * net_poll() models a wedged compositor EXACTLY as far as the network can tell
 * -- and it is a park in this file rather than a sleep in wm.c because that file
 * belongs to another line right now.
 *
 * It is not a stub for the thing under test: the timer path, the softirq and
 * tcp_poll() itself all run untouched while it is armed. What stops is the
 * caller this change exists to make unnecessary. RX keeps arriving, because RX
 * has been on SOFTIRQ_NET since before this change -- which is also what makes
 * the wedge a fair model rather than a total blackout.
 * ------------------------------------------------------------------------- */
static volatile uint64_t park_until_ms;
static uint32_t park_fires0, park_softirq0, park_calls;

/* The measurement, and it is printed by BOTH ways a park can end -- a harness
 * that releases early would otherwise get no numbers at all, which is how an
 * instrument comes to be trusted on the strength of a line saying it was armed.
 * `fires` is the ktimer callback count and `softirq` the passes that actually
 * ran: during a park those are the ONLY drivers, so a park that reports +0/+0
 * and a working transfer means something other than these timers moved the
 * bytes, and the round proved nothing. */
static void park_report(const char *how)
{
    uint32_t f = 0, s = 0;
    net_tcp_timer_stats(&f, &s, 0);
    kprintf("[net] park: %s -- %u net_poll calls refused, tcp timer fires +%u, "
            "softirq passes +%u\n", how, park_calls,
            f - park_fires0, s - park_softirq0);
}

void net_debug_park(long ms)
{
    if (ms <= 0) {
        if (park_until_ms) { park_until_ms = 0; park_report("released by hand"); }
        else kprintf("[net] park: not parked\n");
        return;
    }
    if (ms > 120000) ms = 120000;       /* the WM's own g_net_busy watchdog is 100 s */
    park_calls = 0;
    net_tcp_timer_stats(&park_fires0, &park_softirq0, 0);
    park_until_ms = time_mono_ms() + (uint64_t)ms;
    kprintf("[net] park: net_poll parked for %d ms (the WM's call is gone; "
            "timers_on_wm=%d)\n", (int)ms, tcp_on_wm);
}

/* The runtime half of the negative control: the same variable reachable from a
 * device harness, so ONE boot can measure both wirings.
 *
 * -DTCP_TIMERS_ON_WM REFUSES IT, and that refusal is load-bearing rather than
 * tidy. In that build no ktimer was ever armed, so honouring `on=0` would not
 * restore this change -- it would leave a machine with NO driver for tcp_poll
 * at all, because net_poll() would then be waiting for an owed pass that
 * nothing can ever owe. The wedge gate run against a -DTCP_TIMERS_ON_WM kernel
 * would then fail for a reason that has nothing to do with the property, which
 * is the worst kind of passing control. */
void net_debug_tcp_on_wm(int on)
{
#ifdef TCP_TIMERS_ON_WM
    (void)on;
    kprintf("[net] tcp timers: -DTCP_TIMERS_ON_WM build -- not switchable "
            "(no ktimer was armed)\n");
#else
    tcp_on_wm = on ? 1 : 0;
    kprintf("[net] tcp timers: %s\n",
            tcp_on_wm ? "FORCED BACK ONTO THE WM LOOP (negative control)"
                      : "on the 10 ms ktimer");
#endif
}

/* 1 while parked. */
static int net_parked(void)
{
    if (!park_until_ms) return 0;
    if (time_mono_ms() < park_until_ms) { park_calls++; return 1; }
    park_until_ms = 0;
    park_report("expired");
    return 0;
}

void net_poll(void)
{
    if (net_parked()) return;
    if (up) {
        /* Still the backstop, and still first: a polled-only card has no other
         * receive path at all, and even an IRQ-driven one can have a raise
         * pending from a nested entry (see net_rx_schedule). */
        net_rx_drain(&rx_n_poll);
        rx_report();
        /* NOT a poll any more: discharge an owed timer pass, if the 10 ms
         * ktimer's raise has not been able to run (a nested entry -- see the
         * timer block above). Under the negative-control wiring this is the
         * unconditional call it always was. */
        if (tcp_on_wm) { if (tcp_poll) tcp_poll(); }
        else           tcp_tick_run(&tcp_tick_inline);
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
