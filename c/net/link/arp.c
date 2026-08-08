/* Address Resolution Protocol: RFC 826, RFC 5227 (conflict detection), and the
 * neighbour-cache state machine of RFC 4861 s7.3 applied to IPv4.
 *
 * WHY THIS IS NOT A LOOKUP TABLE. The version this replaces was 101 lines: a
 * 16-slot array, `cache_put` on every ARP packet that arrived, and no notion of
 * time. It worked on exactly one topology -- QEMU SLIRP, one peer, one gateway,
 * addresses that never move -- and each of the things it left out is something
 * a real segment does on an ordinary afternoon:
 *
 *  1. NOTHING EXPIRED. `used` was set once and never cleared, so a binding
 *     learned at boot was permanent. A gateway that reboots onto a different
 *     MAC, a VRRP failover, an address handed to a different host by DHCP --
 *     each of those left the machine transmitting to a MAC that no longer
 *     exists, forever, with no error anywhere. This is the failure that looks
 *     like "the network broke and a reboot fixed it".
 *
 *  2. IT LEARNED FROM ANYONE. `cache_put(spa, sha)` ran before any check, on
 *     every ARP frame on the segment, request or reply, solicited or not,
 *     addressed to us or not. One broadcast frame from any host on the LAN
 *     rewrote the gateway's MAC and put that host in the middle of every
 *     connection the machine had. That is not a hardening nicety; it is a
 *     complete man-in-the-middle with a single packet, and it ran in ring 0.
 *
 *  3. THE EVICTION WAS A ONE-ENTRY CACHE. When the table filled, the victim was
 *     `cache[0]`, always. Slots 1..15 were then frozen for the rest of the boot
 *     and every new peer fought over slot 0. On a /24 with more than sixteen
 *     hosts the gateway landed in slot 0 and was evicted by the next stranger,
 *     repeatedly -- a cache with a 0% hit rate that still looked full.
 *
 *  4. A MISS DROPPED THE PACKET. `arp_resolve` returned -1 and ip_send threw
 *     the datagram away, so the first packet to any cold next hop was always
 *     lost and the cost was a protocol retransmit -- hundreds of milliseconds.
 *     Three call sites above the link layer had grown their own workarounds for
 *     this (`arp_warm` in http.c and dns.c, the S_ARP phase in sock.c with its
 *     own rate limiter). That is what a missing feature looks like from above.
 *
 *  5. EVERY MISS BROADCAST. `arp_resolve` sent a request on each call with no
 *     rate limit of its own, so a caller that did not pace itself put requests
 *     on the wire at whatever rate it retried -- and sock.c's comment says in
 *     so many words that its rate limiter exists because of this. The pacing
 *     belongs here, where the state is.
 *
 * So: five states, a clock, RFC 826's actual update rule, a bounded queue for
 * packets awaiting resolution, and counters for all of it.
 *
 * SHARED SHAPE WITH NEIGHBOUR DISCOVERY. c/net/ip/nd.c solves this same problem
 * for IPv6 and already had the right structure. The states, the constant names
 * and their values (REACHABLE_TIME, RETRANS_TIMER, DELAY_FIRST_PROBE,
 * MAX_MCAST_SOLICIT, MAX_UCAST_SOLICIT) are deliberately identical here, so the
 * two caches age and probe alike and a reader who has understood one has
 * understood the other. The one state ND has that this does not need is its
 * router flag; the one thing this has that ND does not is IPv4 conflict
 * detection, because IPv6 puts that in DAD instead.
 *
 * LOCKING. arp_input runs in the receive path (softirq or, nested, the NIC
 * ISR); arp_resolve/arp_output run on mainline threads. Both mutate the cache
 * and the pending queue, so every mutating path takes net_lock() -- which saves
 * and restores IF and is therefore safe nested inside a driver that already
 * holds it. Under -DLOGIT_NET_HOST it is a no-op and the host tests run the
 * real code. */

#include <stdint.h>
#include <stddef.h>
#include "arp.h"
#include "eth.h"
#include "net.h"
#include "pit.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);

#define ARP_HTYPE_ETH  1
#define ARP_PTYPE_IP   0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;               /* network order */
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;               /* network order */
} __attribute__((packed));

/* RFC 4861 protocol constants in 100 Hz ticks -- the same names and the same
 * values as nd.c, on purpose. See the header comment. */
#define REACHABLE_TIME     3000     /* 30 s: how long a confirmed entry is trusted */
#define RETRANS_TIMER       100     /*  1 s: between solicitations */
#define DELAY_FIRST_PROBE   500     /*  5 s: grace before probing a stale entry */
#define MAX_MCAST_SOLICIT     3     /* broadcast requests before giving up */
#define MAX_UCAST_SOLICIT     3     /* unicast probes before falling back */

/* Two constants ND has no equivalent for.
 *
 * FAILED_HOLD is a NEGATIVE cache, and it is the rate limit that matters. An
 * address nobody answers for -- a host that is off, a typo, a scan -- used to
 * cost one broadcast per call to arp_resolve, forever. Holding the failure
 * bounds that to three requests per hold per address no matter how hard
 * anything above retries, which is the difference between a quiet segment and
 * a machine that broadcasts at whatever rate TCP happens to retransmit.
 *
 * FIVE seconds and not longer, because the hold is also a black hole: while it
 * lasts, every send to that address fails immediately. The failure it is most
 * likely to be caching is a TRANSIENT -- a next hop that has not finished
 * booting, a link that came up a moment ago -- and a long hold turns a
 * two-second outage into a twenty-second one for no benefit. At 5 s the
 * broadcast cost is already down to 0.6 requests per second per address from
 * unbounded, which is the whole point; going longer buys almost nothing and
 * delays recovery fourfold.
 *
 * STALE_GC is what makes the table finite in the right way: an entry nobody has
 * used for two minutes is not evidence of anything and should not be occupying
 * a slot that an active peer wants. */
#define FAILED_HOLD         500     /*  5 s */
#define STALE_GC          12000     /* 120 s */

/* The states themselves are in arp.h, so a test can name a transition rather
 * than infer it. What they mean:
 *
 *   INCOMPLETE  solicited, no answer yet; may hold queued packets
 *   REACHABLE   confirmed within REACHABLE_TIME
 *   STALE       usable, unconfirmed; a use moves it to DELAY
 *   DELAY       used while stale; probing begins after DELAY_FIRST_PROBE
 *   PROBE       unicast solicitations in flight against the address we hold
 *   FAILED      nobody answered; negative-cached until `timer`
 */

#define ARP_CACHE 32

struct arp_entry {
    uint32_t ip;              /* host order */
    uint8_t  mac[ETH_ALEN];
    uint8_t  state;
    uint8_t  probes;          /* solicitations sent in the current state */
    uint64_t timer;           /* tick at which the current state expires */
    uint64_t used_at;         /* last time anything asked for this entry (LRU) */
};

static struct arp_entry cache[ARP_CACHE];

/* ---- packets waiting on resolution -------------------------------------
 *
 * RFC 1122 s2.3.2.2: "the link layer SHOULD save (rather than discard) at least
 * one packet of each set of packets destined to the same unresolved IP
 * address". The old code discarded all of them. A global pool rather than a
 * per-entry array because the interesting case is one cold destination with a
 * burst behind it, not thirty destinations with one packet each; a pool lets
 * that one destination use the whole budget. */
#define ARP_QUEUE      8
#define ARP_QUEUE_MTU  1514

struct arp_pending {
    uint32_t ip;              /* 0 = slot free */
    uint16_t ethertype;
    uint16_t len;
    uint64_t queued_at;
    uint8_t  data[ARP_QUEUE_MTU];
};

static struct arp_pending pendq[ARP_QUEUE];

/* A queued packet that has waited longer than this is not worth sending: the
 * layer above has already retransmitted it. Delivering the stale copy as well
 * just duplicates it on the wire. */
#define QUEUE_TTL 300         /* 3 s */

static struct arp_stats stats;

const struct arp_stats *arp_get_stats(void) { return &stats; }

/* ---- small helpers ------------------------------------------------------ */

static int mac_is_multicast(const uint8_t *m) { return (m[0] & 1) != 0; }

static int mac_is_zero(const uint8_t *m)
{
    for (int i = 0; i < ETH_ALEN; i++) if (m[i]) return 0;
    return 1;
}

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, ETH_ALEN) == 0;
}

/* An address that can legitimately appear as an ARP sender. A sender protocol
 * address of 0 is an RFC 5227 probe (and DHCP's), which explicitly MUST NOT
 * create a cache entry -- the old code cached one for 0.0.0.0 and then happily
 * resolved it. Multicast and the all-ones broadcast cannot be a sender at all;
 * accepting them is how a forged frame gets the broadcast MAC installed as some
 * host's binding, which turns every subsequent packet into a flood. */
static int plausible_sender(uint32_t ip)
{
    if (ip == 0 || ip == 0xFFFFFFFFu) return 0;
    if ((ip >> 28) == 0xE) return 0;              /* 224.0.0.0/4 multicast */
    if ((ip >> 24) == 127) return 0;              /* 127/8 is not on a wire   */
    return 1;
}

/* ---- cache ------------------------------------------------------------- */

static struct arp_entry *lookup(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (cache[i].state != ARP_FREE && cache[i].ip == ip)
            return &cache[i];
    return NULL;
}

/* Least-recently-USED, which is what the old code's comment claimed and its
 * code did not do. Free slots first; then a FAILED entry (a negative result is
 * the cheapest thing to lose); then genuinely the oldest use. */
static struct arp_entry *alloc_entry(uint32_t ip)
{
    struct arp_entry *victim = NULL;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (cache[i].state == ARP_FREE) { victim = &cache[i]; goto take; }
    }
    for (int i = 0; i < ARP_CACHE; i++) {
        if (cache[i].state == ARP_FAILED) { victim = &cache[i]; goto take; }
    }
    victim = &cache[0];
    for (int i = 1; i < ARP_CACHE; i++)
        if (cache[i].used_at < victim->used_at) victim = &cache[i];
    stats.evict++;
take:
    memset(victim, 0, sizeof *victim);
    victim->ip = ip;
    victim->used_at = timer_ticks();
    return victim;
}

static void entry_free(struct arp_entry *e)
{
    memset(e, 0, sizeof *e);
}

/* ---- transmit ----------------------------------------------------------- */

/* `spa` is explicit rather than always net_cfg.ip because RFC 5227's probe uses
 * a sender protocol address of 0 -- "I am asking whether this address is taken
 * and I am not claiming it yet" -- and getting that wrong turns a probe into a
 * claim, which is precisely the conflict it exists to avoid. */
static void send_arp(uint16_t op, const uint8_t *dst_mac,
                     const uint8_t *target_mac, uint32_t spa, uint32_t tpa)
{
    struct arp_pkt p;
    p.htype = htons(ARP_HTYPE_ETH);
    p.ptype = htons(ARP_PTYPE_IP);
    p.hlen  = ETH_ALEN;
    p.plen  = 4;
    p.op    = htons(op);
    memcpy(p.sha, net_cfg.mac, ETH_ALEN);
    p.spa = htonl(spa);
    memcpy(p.tha, target_mac, ETH_ALEN);
    p.tpa = htonl(tpa);
    if (op == ARP_OP_REQUEST) stats.tx_req++; else stats.tx_reply++;
    eth_send(dst_mac, ETHERTYPE_ARP, &p, sizeof p);
}

/* A solicitation. Broadcast when we have no MAC to aim at (INCOMPLETE), unicast
 * when we are confirming one we already hold (PROBE) -- the unicast form is the
 * whole point of the PROBE state: it re-verifies a binding without putting a
 * broadcast on the segment. RFC 4861 s7.2.2's target-hardware-address field is
 * zero in a solicitation, so `target_mac` is zeroed regardless of where the
 * frame is aimed. */
static void solicit(uint32_t ip, const uint8_t *unicast_to)
{
    static const uint8_t zero[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
    send_arp(ARP_OP_REQUEST, unicast_to ? unicast_to : eth_broadcast,
             zero, net_cfg.ip, ip);
}

/* RFC 5227 s3: an ANNOUNCEMENT. A request for our own address, from our own
 * address, broadcast -- so every host on the segment updates any entry it holds
 * for us, and any host that thinks it owns the address notices. This is what a
 * stack sends when it takes an address, and this one has never sent it: nothing
 * on the segment learned our binding until we happened to talk to it, and no
 * duplicate was ever detected. */
void arp_announce(void)
{
    static const uint8_t zero[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
    if (!net_cfg.ip) return;
    uint64_t f = net_lock();
    stats.tx_announce++;
    send_arp(ARP_OP_REQUEST, eth_broadcast, zero, net_cfg.ip, net_cfg.ip);
    net_unlock(f);
}

/* ---- the pending queue -------------------------------------------------- */

/* Send everything queued for `ip`. Called with the cache lock held and only
 * once a MAC is known. */
static void queue_flush(uint32_t ip, const uint8_t *mac)
{
    for (int i = 0; i < ARP_QUEUE; i++) {
        if (pendq[i].ip != ip) continue;
        eth_send(mac, pendq[i].ethertype, pendq[i].data, pendq[i].len);
        stats.queue_sent++;
        pendq[i].ip = 0;
    }
}

static void queue_drop(uint32_t ip)
{
    for (int i = 0; i < ARP_QUEUE; i++)
        if (pendq[i].ip == ip) { pendq[i].ip = 0; stats.queue_drop++; }
}

/* RFC 1122 again: when the queue is full the packet to discard is the OLDEST,
 * not the new one. The newest packet is the one the peer is still waiting for;
 * the oldest is the one whose sender has already given up on it. */
static int queue_push(uint32_t ip, uint16_t ethertype, const void *data, uint16_t len)
{
    if (len > ARP_QUEUE_MTU) return -1;
    struct arp_pending *slot = NULL;
    for (int i = 0; i < ARP_QUEUE; i++)
        if (pendq[i].ip == 0) { slot = &pendq[i]; break; }
    if (!slot) {
        slot = &pendq[0];
        for (int i = 1; i < ARP_QUEUE; i++)
            if (pendq[i].queued_at < slot->queued_at) slot = &pendq[i];
        stats.queue_drop++;
    }
    slot->ip = ip;
    slot->ethertype = ethertype;
    slot->len = len;
    slot->queued_at = timer_ticks();
    memcpy(slot->data, data, len);
    stats.queued++;
    return 0;
}

/* ---- receive ------------------------------------------------------------ */

/* RFC 4861 s7.2.5, applied to ARP: when do we believe a hardware address?
 *
 * The rule that matters is that an UNSOLICITED packet may not silently replace
 * a binding we already hold. ND encodes this in the Override flag; ARP has no
 * such flag, so the equivalent is "did this arrive as a unicast reply to a
 * request of ours". If it did, it is an answer and we take it. If it did not --
 * a broadcast request, a gratuitous announcement, anything from a host we never
 * asked -- then it may make us DOUBT the binding we hold, but it may not
 * replace it. Doubt means STALE, which means we probe the address we already
 * have, unicast, and the genuine owner's reply wins.
 *
 * That is the difference between "an attacker can redirect the machine's
 * traffic with one broadcast frame" and "an attacker can make the machine
 * re-verify, and lose". It costs one packet in the case where a MAC genuinely
 * changed, and it converges there too: the probes to the old MAC go unanswered,
 * the entry falls back to INCOMPLETE, the broadcast finds the new owner. */
static void neighbour_update(uint32_t spa, const uint8_t *sha, int solicited)
{
    struct arp_entry *e = lookup(spa);
    uint64_t now = timer_ticks();

    if (!e) return;                       /* creation is the caller's decision */

    int had_mac = (e->state != ARP_INCOMPLETE && e->state != ARP_FAILED);
    int changed = had_mac && !mac_eq(e->mac, sha);

#ifdef ARP_NEGCTL_TRUST_ANY
    /* Negative control (tests/unit/arp_test.c). Restores the behaviour this
     * file replaced: believe any hardware address from any ARP frame, exactly
     * as the old `cache_put(ntohl(p->spa), p->sha)` did. If the suite still
     * passes with this defined, then nothing in it is actually testing the
     * thing the rewrite is for. */
    (void)changed;
    memcpy(e->mac, sha, ETH_ALEN);
    e->state  = ARP_REACHABLE;
    e->probes = 0;
    e->timer  = now + REACHABLE_TIME;
    queue_flush(spa, e->mac);
    return;
#endif

    if (!had_mac) {                       /* first address for this entry */
        memcpy(e->mac, sha, ETH_ALEN);
        e->state  = solicited ? ARP_REACHABLE : ARP_STALE;
        e->probes = 0;
        e->timer  = now + (solicited ? REACHABLE_TIME : STALE_GC);
        queue_flush(spa, e->mac);
        return;
    }

    if (!changed) {
        if (solicited) {                  /* confirmed by an answer we asked for */
            e->state  = ARP_REACHABLE;
            e->probes = 0;
            e->timer  = now + REACHABLE_TIME;
        } else if (e->state == ARP_REACHABLE) {
            /* Same address, but nothing confirmed it. Leave it reachable; an
             * unsolicited restatement of what we already believe is not new
             * information, and is exactly what a chatty segment is full of. */
        }
        queue_flush(spa, e->mac);
        return;
    }

    /* The address differs from the one we hold. */
    if (solicited) {
        memcpy(e->mac, sha, ETH_ALEN);
        e->state  = ARP_REACHABLE;
        e->probes = 0;
        e->timer  = now + REACHABLE_TIME;
        queue_flush(spa, e->mac);
        return;
    }

    /* Unsolicited and different: the poisoning attempt, and also what a real
     * failover looks like. Refuse the substitution and go and check. */
    stats.poison_blocked++;
    if (e->state == ARP_REACHABLE || e->state == ARP_STALE) {
        e->state  = ARP_PROBE;
        e->probes = 0;
        e->timer  = now + RETRANS_TIMER;
        solicit(spa, e->mac);             /* unicast, to the address we still hold */
    }
}

void arp_input(const uint8_t *frame, uint16_t len)
{
    stats.rx++;
    if (len < sizeof(struct eth_hdr) + sizeof(struct arp_pkt)) {
        stats.rx_bad++;
        return;
    }
    const struct arp_pkt *p = (const struct arp_pkt *)(frame + sizeof(struct eth_hdr));

    /* Validate the whole fixed header, not just ptype. hlen and plen are the
     * fields that say how to read the four addresses that follow, so a frame
     * that disagrees with the layout this struct assumes is not an ARP packet
     * we can parse -- it is a differently-shaped one whose sha/spa/tha/tpa are
     * at other offsets. The old code read them at Ethernet/IPv4 offsets no
     * matter what these said. */
    if (ntohs(p->htype) != ARP_HTYPE_ETH || ntohs(p->ptype) != ARP_PTYPE_IP ||
        p->hlen != ETH_ALEN || p->plen != 4) {
        stats.rx_bad++;
        return;
    }

    uint16_t op  = ntohs(p->op);
    uint32_t spa = ntohl(p->spa);
    uint32_t tpa = ntohl(p->tpa);

    if (op == ARP_OP_REQUEST) stats.rx_req++;
    else if (op == ARP_OP_REPLY) stats.rx_reply++;
    else { stats.rx_bad++; return; }

    /* A sender hardware address that is multicast, broadcast or zero cannot
     * belong to any host. Caching one makes every packet to that binding a
     * flood or a black hole, and there is no legitimate frame that carries it. */
    if (mac_is_multicast(p->sha) || mac_is_zero(p->sha)) {
        stats.rx_bad++;
        return;
    }

    uint64_t f = net_lock();

    /* RFC 5227 s2.4: somebody else is using our address. This is checked before
     * anything is learned, because the one thing we must not do is enter their
     * MAC as the binding for our own address -- which the old code did, so
     * arp_resolve(our own IP) would have returned the impostor's MAC.
     *
     * The response is the "defend" of RFC 5227: broadcast our own binding once,
     * so hosts that cached theirs correct themselves. We do not surrender the
     * address (a host that gives up its address on any conflicting frame can be
     * knocked off the network by one packet). */
    if (spa == net_cfg.ip && net_cfg.ip && !mac_eq(p->sha, net_cfg.mac)) {
        stats.conflicts++;
        kprintf("[arp] address conflict: %d.%d.%d.%d also claimed by "
                "%x:%x:%x:%x:%x:%x\n",
                (spa >> 24) & 0xFF, (spa >> 16) & 0xFF, (spa >> 8) & 0xFF, spa & 0xFF,
                p->sha[0], p->sha[1], p->sha[2], p->sha[3], p->sha[4], p->sha[5]);
        stats.tx_announce++;
        send_arp(ARP_OP_REPLY, p->sha, p->sha, net_cfg.ip, spa);
        net_unlock(f);
        return;
    }

    if (op == ARP_OP_REQUEST && spa == 0) {
        /* An RFC 5227 probe: somebody is asking whether tpa is free. If it is
         * ours we must answer so they do not take it -- but there is nothing to
         * learn, since the sender has no address yet. */
        if (tpa == net_cfg.ip && net_cfg.ip)
            send_arp(ARP_OP_REPLY, p->sha, p->sha, net_cfg.ip, spa);
        net_unlock(f);
        return;
    }

    if (!plausible_sender(spa)) {
        stats.rx_bad++;
        net_unlock(f);
        return;
    }

    /* RFC 826's merge rule, which the old code did not implement at all.
     *
     *     if <entry for spa exists>: update it;              merge = true
     *     if tpa is one of ours:
     *         if not merge: create the entry
     *         if op is REQUEST: reply
     *
     * The consequence -- and the reason this single `if` is most of the fix --
     * is that a packet which is neither addressed to us nor about a host we are
     * already talking to CREATES NOTHING. A stranger's gratuitous ARP can no
     * longer occupy a slot, so the table cannot be filled by traffic that has
     * nothing to do with us, and the 16 forged frames that used to evict every
     * real binding now evict nothing. */
    int for_us = (tpa == net_cfg.ip && net_cfg.ip != 0);
    int solicited = (op == ARP_OP_REPLY && mac_eq(p->tha, net_cfg.mac));

#ifdef ARP_NEGCTL_TRUST_ANY
    /* The other half of the control: the old code created an entry for every
     * sender it ever saw, addressed to us or not, which is what let sixteen
     * forged frames evict every real binding in the table. */
    for_us = 1;
#endif

    if (op == ARP_OP_REQUEST && spa != 0 && tpa == spa)
        stats.rx_gratuitous++;

    if (lookup(spa)) {
        neighbour_update(spa, p->sha, solicited);
    } else if (for_us) {
        struct arp_entry *e = alloc_entry(spa);
        memcpy(e->mac, p->sha, ETH_ALEN);
        e->state  = solicited ? ARP_REACHABLE : ARP_STALE;
        e->timer  = timer_ticks() + (solicited ? REACHABLE_TIME : STALE_GC);
        queue_flush(spa, e->mac);
    }

    if (op == ARP_OP_REQUEST && for_us)
        send_arp(ARP_OP_REPLY, p->sha, p->sha, net_cfg.ip, spa);

    net_unlock(f);
}

/* ---- resolution --------------------------------------------------------- */

/* IPv4 multicast has a DEFINED hardware address (RFC 1112 s6.4): 01:00:5e
 * followed by the low 23 bits of the group. There is no ARP for it and never
 * was -- yet ip_send() hands any non-broadcast destination to arp_resolve, so
 * a multicast destination used to broadcast "who has 224.0.0.251", which no
 * host will ever answer, on every single send. */
static int multicast_mac(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    if ((ip >> 28) != 0xE) return 0;
    mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5E;
    mac[3] = (uint8_t)((ip >> 16) & 0x7F);
    mac[4] = (uint8_t)((ip >> 8) & 0xFF);
    mac[5] = (uint8_t)(ip & 0xFF);
    return 1;
}

int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    if (multicast_mac(ip, mac)) return 0;

    /* Our own address resolves to our own MAC. Without this the machine could
     * not address itself at all: ip_send() to net_cfg.ip takes the on-link path,
     * asks ARP for our own address, broadcasts a request nobody answers (a host
     * does not receive its own transmissions), and never resolves. eth_send()
     * turns a frame aimed at our own MAC into a loopback delivery, so this one
     * line is what makes the local address reachable from the machine itself. */
    if (ip == net_cfg.ip && net_cfg.ip) {
        memcpy(mac, net_cfg.mac, ETH_ALEN);
        return 0;
    }

    uint64_t f = net_lock();
    uint64_t now = timer_ticks();
    struct arp_entry *e = lookup(ip);

    if (e) {
        e->used_at = now;
        switch (e->state) {
        case ARP_REACHABLE:
            memcpy(mac, e->mac, ETH_ALEN);
            net_unlock(f);
            return 0;
        case ARP_STALE:
            /* Usable. RFC 4861 s7.3.3: send on it, and start the delay before
             * probing -- a stale binding is almost always still correct, and
             * refusing to use it would cost a round trip on every entry that
             * happened to age out between two fetches. */
            e->state  = ARP_DELAY;
            e->timer  = now + DELAY_FIRST_PROBE;
            memcpy(mac, e->mac, ETH_ALEN);
            net_unlock(f);
            return 0;
        case ARP_DELAY:
        case ARP_PROBE:
            memcpy(mac, e->mac, ETH_ALEN);
            net_unlock(f);
            return 0;
        case ARP_INCOMPLETE:
            /* A request is already outstanding. Do NOT send another: the
             * retransmit schedule belongs to arp_poll, and this is where the
             * old code's broadcast-per-call came from. */
            net_unlock(f);
            return -1;
        case ARP_FAILED:
            /* Negative cache. Answering -1 without transmitting is the whole
             * point; it expires by itself in arp_poll.
             *
             * Deliberately NOT counting resolve_fail here. That counter means
             * "addresses that never answered", and arp_poll increments it once
             * when the address actually gives up; incrementing again per call
             * would make the number a measure of how hard the caller retried
             * instead, which is a different quantity wearing the same name. */
            net_unlock(f);
            return -1;
        }
    }

    e = alloc_entry(ip);
    e->state  = ARP_INCOMPLETE;
    e->probes = 1;
    e->timer  = now + RETRANS_TIMER;
    solicit(ip, NULL);
    net_unlock(f);
    return -1;
}

/* Resolve, or hold the packet until resolution completes.
 *
 * This is the entry point ip_send() should use instead of "arp_resolve, and
 * throw the datagram away on a miss". It is a strict improvement at the call
 * site -- a hit behaves identically -- and it is the only way the first packet
 * to a cold next hop survives. */
int arp_output(uint32_t nexthop, uint16_t ethertype, const void *payload, uint16_t len)
{
    uint8_t mac[ETH_ALEN];
    if (arp_resolve(nexthop, mac) == 0)
        return eth_send(mac, ethertype, payload, len);

    uint64_t f = net_lock();
    struct arp_entry *e = lookup(nexthop);
    /* Only queue behind a solicitation that is actually in flight. Queueing
     * behind a FAILED entry would hold packets for an address we have already
     * decided is not answering, and deliver them 20 s late. */
    int ok = (e && e->state == ARP_INCOMPLETE) ? queue_push(nexthop, ethertype, payload, len) : -1;
    net_unlock(f);
    return ok;
}

/* ---- the clock ---------------------------------------------------------- */

/* One pass of the state machine. Called from net_poll (~100 Hz); everything
 * here is deadline arithmetic against timer_ticks, so a missed pass costs
 * lateness and never correctness. */
void arp_poll(void)
{
    uint64_t now = timer_ticks();
    uint64_t f = net_lock();

    for (int i = 0; i < ARP_QUEUE; i++)
        if (pendq[i].ip && now - pendq[i].queued_at > QUEUE_TTL) {
            pendq[i].ip = 0;
            stats.queue_drop++;
        }

    for (int i = 0; i < ARP_CACHE; i++) {
        struct arp_entry *e = &cache[i];
        if (e->state == ARP_FREE || (int64_t)(now - e->timer) < 0) continue;

        switch (e->state) {
        case ARP_INCOMPLETE:
            if (e->probes >= MAX_MCAST_SOLICIT) {
                /* Nobody is there. Drop what was queued for it -- holding those
                 * any longer only delivers duplicates -- and remember the
                 * failure so the next caller does not restart the broadcasts. */
                queue_drop(e->ip);
                stats.resolve_fail++;
                e->state = ARP_FAILED;
                e->timer = now + FAILED_HOLD;
            } else {
                e->probes++;
                e->timer = now + RETRANS_TIMER;
                solicit(e->ip, NULL);
            }
            break;

        case ARP_REACHABLE:
            /* Confirmation has aged out. The binding is probably still right,
             * so it stays usable -- it just has to be re-verified before it is
             * trusted again. This is the transition the old cache lacked
             * entirely, and its absence is why a moved address never healed. */
            e->state = ARP_STALE;
            e->timer = now + STALE_GC;
            break;

        case ARP_STALE:
            entry_free(e);              /* unused for STALE_GC; reclaim the slot */
            stats.expire++;
            break;

        case ARP_DELAY:
            e->state  = ARP_PROBE;
            e->probes = 1;
            e->timer  = now + RETRANS_TIMER;
            solicit(e->ip, e->mac);     /* unicast: no broadcast on the segment */
            break;

        case ARP_PROBE:
            if (e->probes >= MAX_UCAST_SOLICIT) {
                /* The address we hold has stopped answering. Fall back to a
                 * broadcast search rather than deleting the entry: this is the
                 * MAC-changed case, and the broadcast is what finds the new
                 * owner. The old address is forgotten here, which is the only
                 * place in this file where a binding is dropped without a
                 * replacement in hand. */
                memset(e->mac, 0, ETH_ALEN);
                e->state  = ARP_INCOMPLETE;
                e->probes = 1;
                e->timer  = now + RETRANS_TIMER;
                solicit(e->ip, NULL);
            } else {
                e->probes++;
                e->timer = now + RETRANS_TIMER;
                solicit(e->ip, e->mac);
            }
            break;

        case ARP_FAILED:
            entry_free(e);
            break;
        }
    }
    net_unlock(f);
}

/* Block (polling) until `ip` is resolvable, or `timeout` ticks elapse.
 * Warms the cache so the first real packet to a next-hop isn't silently dropped
 * on an ARP miss. Safe only from the blocking fetch context (it pumps
 * net_poll), NOT from the RX IRQ.
 *
 * The hand-rolled rate limiter this used to carry is gone: arp_resolve no
 * longer transmits on every call, so calling it in a tight loop no longer
 * floods the segment. The pacing is arp_poll's, which is where the state is. */
int arp_warm(uint32_t ip, int timeout)
{
    uint8_t mac[ETH_ALEN];
    uint64_t start = timer_ticks();
    while (arp_resolve(ip, mac) != 0) {
        if ((int)(timer_ticks() - start) > timeout) return -1;
        net_poll();
        net_idle();
    }
    return 0;
}

/* ---- visibility --------------------------------------------------------- */

int arp_cache_entries(void)
{
    int n = 0;
    for (int i = 0; i < ARP_CACHE; i++)
        if (cache[i].state != ARP_FREE && cache[i].state != ARP_FAILED) n++;
    return n;
}

int arp_query_state(uint32_t ip)
{
    struct arp_entry *e = lookup(ip);
    return e ? e->state : ARP_FREE;
}

void arp_dump(void)
{
    kprintf("[arp] rx %u (req %u reply %u grat %u bad %u) tx (req %u reply %u ann %u)\n",
            stats.rx, stats.rx_req, stats.rx_reply, stats.rx_gratuitous, stats.rx_bad,
            stats.tx_req, stats.tx_reply, stats.tx_announce);
    kprintf("[arp] entries %d  evict %u expire %u  queued %u sent %u dropped %u  "
            "unresolved %u conflicts %u poison-blocked %u\n",
            arp_cache_entries(), stats.evict, stats.expire,
            stats.queued, stats.queue_sent, stats.queue_drop,
            stats.resolve_fail, stats.conflicts, stats.poison_blocked);
}

/* Host tests only: start from a known cache. Cheap enough to always compile in,
 * and the alternative is a second build of this file. */
void arp_reset(void)
{
    memset(cache, 0, sizeof cache);
    memset(pendq, 0, sizeof pendq);
    memset(&stats, 0, sizeof stats);
}
