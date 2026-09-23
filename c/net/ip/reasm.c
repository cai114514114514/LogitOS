#include <stdint.h>
#include <stddef.h>
#include "reasm.h"
#include "net.h"
#include "pit.h"
#include "../../../include/weaksym.h"   /* the weak ip6_poll below is an ELF idiom */

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

#define REASM_SLOTS   4
#define REASM_MAX     65536      /* 64 KiB payload cap per datagram */
#define REASM_HDR     60         /* max IPv4 header (IHL is a 4-bit count) */
#define REASM_TIMEOUT 3000       /* ticks: a stalled datagram dies after ~30 s */

struct reasm_slot {
    int      used;
    uint32_t src, dst;           /* key, host order (dst matters: broadcast too) */
    uint16_t id;                 /* key */
    uint8_t  proto;              /* key */
    uint64_t tick;               /* last fragment arrival */
    uint32_t total;              /* payload length, once the MF=0 fragment is seen */
    int      have_last;
    uint32_t rcvd;               /* bytes so far; no overlaps within the stated
                                  * total makes == total prove full coverage */
    uint32_t high;               /* one past the highest received payload byte */
    uint8_t  hdr[REASM_HDR];     /* offset-0 fragment's header: its dst feeds the
                                  * UDP pseudo-header (broadcast) and it is what
                                  * an ICMP error would quote */
    uint8_t  hdr_len;
    uint8_t  data[REASM_MAX];
    uint8_t  map[REASM_MAX / 8]; /* per-byte received bitmap */
};

static struct reasm_slot slots[REASM_SLOTS];

static int byte_rcvd(const struct reasm_slot *s, uint32_t i)
{
    return s->map[i >> 3] & (uint8_t)(1u << (i & 7));
}

static struct reasm_slot *find_slot(uint32_t src, uint32_t dst,
                                    uint8_t proto, uint16_t id)
{
    for (int i = 0; i < REASM_SLOTS; i++)
        if (slots[i].used && slots[i].src == src && slots[i].dst == dst &&
            slots[i].id == id && slots[i].proto == proto)
            return &slots[i];
    return NULL;
}

int reasm_input(uint32_t src, uint32_t dst, uint8_t proto, uint16_t id,
                const uint8_t *iph, uint8_t ihl,
                uint16_t off, int more, const uint8_t *data, uint16_t dlen,
                struct reasm_dgram *out)
{
    if (dlen == 0)                       /* carries no information; ignore */
        return 0;
    struct reasm_slot *s = find_slot(src, dst, proto, id);
    if (!s) {
        for (int i = 0; i < REASM_SLOTS; i++)
            if (!slots[i].used) { s = &slots[i]; break; }
        if (!s)
            return 0;                    /* all slots busy: drop, don't evict */
        memset(s, 0, offsetof(struct reasm_slot, data));
        memset(s->map, 0, sizeof s->map);
        s->used = 1;
        s->src = src; s->dst = dst; s->id = id; s->proto = proto;
    }
    s->tick = timer_ticks();

    /* Any byte beyond the buffer, any overlap with already-received bytes, or
     * a conflicting tail poisons the whole datagram. The old claim above said
     * no overlaps made rcvd == total sufficient. It missed bytes past a later
     * tail: [64,72) plus [8,16) has rcvd == total == 16, but [0,8) is absent.
     * Refuse that tail before the byte count can call stale slot data complete.
     * Silently merging overlaps is the teardrop-class attack surface too. */
    uint32_t end = (uint32_t)off + dlen;
    if (end > REASM_MAX ||
        (s->have_last && end > s->total) ||
        (!more && s->have_last && end != s->total)
#ifndef REASM_NEGCTL_SHORT_TAIL
        || (!more && end < s->high)
#endif
        ) {
        s->used = 0;
        return 0;
    }
    for (uint32_t i = off; i < end; i++)
        if (byte_rcvd(s, i)) { s->used = 0; return 0; }

    memcpy(s->data + off, data, dlen);
    for (uint32_t i = off; i < end; i++)
        s->map[i >> 3] |= (uint8_t)(1u << (i & 7));
    s->rcvd += dlen;
    if (end > s->high) s->high = end;
    if (off == 0 && ihl <= REASM_HDR) {  /* keep the first fragment's header */
        memcpy(s->hdr, iph, ihl);
        s->hdr_len = ihl;
    }
    if (!more) {
        s->have_last = 1;
        s->total = end;
    }

    if (s->have_last && s->rcvd == s->total) {
        if (s->total > 0xFFFF) return 0;   /* 65536 wraps the u16 l4len to 0
                                              (2026-09-16 audit) */
        out->iph = s->hdr;
        out->l4 = s->data;
        out->l4len = (uint16_t)s->total;
        out->slot = (int)(s - slots);
        return 1;
    }
    return 0;
}

void reasm_release(struct reasm_dgram *g)
{
    if (g->slot >= 0 && g->slot < REASM_SLOTS)
        slots[g->slot].used = 0;
    g->slot = -1;
}

void ip6_poll(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(ip6_poll);

void ip_poll(void)
{
    uint64_t f = net_lock();        /* the RX IRQ feeds slots concurrently */
    uint64_t now = timer_ticks();
    for (int i = 0; i < REASM_SLOTS; i++)
        if (slots[i].used && now - slots[i].tick > REASM_TIMEOUT)
            slots[i].used = 0;
    net_unlock(f);
    /* The IPv6 timer pump rides here rather than in net_poll(): ip_poll is
     * already the network layer's per-tick hook and c/net/core/net.c belongs to
     * another line. Everything IPv6 that is time-driven -- Duplicate Address
     * Detection, the neighbour cache's REACHABLE/STALE/DELAY/PROBE
     * transitions, Router Solicitation retransmission, address and router
     * lifetimes -- is behind this one call. */
    if (LOGIT_HAVE(ip6_poll)) ip6_poll();
}
