/* The IPv4 forwarding table. See route.h for what it replaces and why it is
 * written in integers only.
 *
 * Freestanding on purpose: no libc, no allocator, no kprintf. The table is a
 * fixed array because the alternative -- kmalloc'ing rows -- would put an
 * allocation on a path that ip_send() calls, and the one thing this file must
 * never do is fail to answer because memory was tight. The whole subsystem is
 * 768 bytes of .bss -- measured, `size -A build/c/net/core/route.o`, and that
 * covers the 16 rows AND the per-interface memo below.
 */

#include <stdint.h>
#include "route.h"

/* A slot, i.e. a row plus the two fields callers have no business setting.
 * `seq` is the insertion order, kept explicitly rather than inferred from the
 * slot index because route_del() frees slots out of order and a reused slot
 * would otherwise jump to the front of the tie-break. */
struct rt_slot {
    struct route_entry e;
    uint8_t  used;
    uint32_t seq;
};

static struct rt_slot tab[RT_NROUTE];
static uint32_t rt_seq;             /* monotonic; never reset except by flush */
static uint32_t rt_gen;

/* Per-interface memo for route_v4_iface. `set` distinguishes "configured with
 * 0.0.0.0" from "never configured", which matters: an interface that is
 * deliberately unnumbered and one that has not been reached yet want different
 * behaviour on the next call. */
struct rt_ifmemo {
    uint32_t addr, mask, gw, flags;
    uint8_t  set;
};
static struct rt_ifmemo ifmemo[RT_NIF];

uint32_t route_generation(void) { return rt_gen; }

uint32_t route_plen_mask(int plen)
{
    if (plen <= 0) return 0;
    if (plen >= 32) return 0xFFFFFFFFu;
    return (uint32_t)(0xFFFFFFFFu << (32 - plen));
}

int route_mask_plen(uint32_t mask)
{
    int n = 0;
    /* Count the leading ones, then require the remainder to be all zero. The
     * two-step form is what rejects 255.255.0.255 instead of reporting 16 for
     * it -- a popcount would happily call that a /24. */
    while (n < 32 && (mask & 0x80000000u)) { n++; mask <<= 1; }
    if (mask) return -1;        /* a bit set after the run of ones is a hole */
    return n;
}

/* ---- table ------------------------------------------------------------- */

static int same_route(const struct route_entry *a, const struct route_entry *b)
{
    return a->dst == b->dst && a->plen == b->plen &&
           a->oif == b->oif && a->nexthop == b->nexthop;
}

int route_add(struct route_entry r)
{
    if (r.plen > 32) return RT_EINVAL;
    if (r.oif <= 0) return RT_EINVAL;
    /* Normalise: 10.0.2.15/24 and 10.0.2.0/24 are the same prefix, and storing
     * the first would make every subsequent compare against the second miss.
     * Linux does the same on `ip route add`. */
    r.dst &= route_plen_mask(r.plen);

    for (int i = 0; i < RT_NROUTE; i++) {
        if (!tab[i].used) continue;
        if (!same_route(&tab[i].e, &r)) continue;
        /* Same route, restated. Update the mutable fields in place and keep
         * the original seq, so re-running a configuration does not silently
         * reorder the tie-break under an unchanged table. */
        tab[i].e.src = r.src;
        tab[i].e.flags = r.flags;
        tab[i].e.metric = r.metric;
        rt_gen++;
        return RT_OK;
    }
    for (int i = 0; i < RT_NROUTE; i++) {
        if (tab[i].used) continue;
        tab[i].e = r;
        tab[i].used = 1;
        tab[i].seq = ++rt_seq;
        rt_gen++;
        return RT_OK;
    }
    return RT_EFULL;
}

int route_del(uint32_t dst, int plen, int oif)
{
    if (plen < 0 || plen > 32) return RT_EINVAL;
    dst &= route_plen_mask(plen);
    int n = 0;
    for (int i = 0; i < RT_NROUTE; i++) {
        if (!tab[i].used) continue;
        if (tab[i].e.dst != dst || tab[i].e.plen != plen) continue;
        if (oif > 0 && tab[i].e.oif != oif) continue;
        tab[i].used = 0;
        n++;
    }
    if (!n) return RT_ENOROUTE;
    rt_gen++;
    return n;
}

int route_lookup(uint32_t dst, struct route_res *out)
{
    int best = -1;

    for (int i = 0; i < RT_NROUTE; i++) {
        if (!tab[i].used) continue;
        const struct route_entry *e = &tab[i].e;
        if ((dst & route_plen_mask(e->plen)) != e->dst) continue;

#ifdef ROUTE_FIRST_MATCH
        /* NEGATIVE CONTROL (tests/route.mk test-route-negctl): the plausible
         * wrong table, not the absent one. Every route still matches correctly
         * and every field is still carried out; the ONLY thing removed is the
         * ranking, so the first matching row in insertion order wins. A table
         * with a default route installed before the connected route -- which
         * is the order any real configuration produces, because the gateway is
         * known at the same moment the address is -- then answers every single
         * lookup with the gateway, exactly as ip.c's old ternary did.
         *
         * This is the control worth having precisely because it still returns
         * a usable-looking answer: an interface, a next hop and a source
         * address, all well-formed. Nothing crashes and nothing is empty. */
        if (best < 0) best = i;
        continue;
#else
        if (best < 0) { best = i; continue; }
        const struct route_entry *b = &tab[best].e;
        if (e->plen != b->plen) { if (e->plen > b->plen) best = i; continue; }
        if (e->metric != b->metric) { if (e->metric < b->metric) best = i; continue; }
        if (tab[i].seq < tab[best].seq) best = i;
#endif
    }

    if (best < 0) return RT_ENOROUTE;
    if (out) {
        const struct route_entry *e = &tab[best].e;
        out->oif = e->oif;
        /* Resolve on-link here and nowhere else. Leaving `nexthop == 0` for
         * the caller to interpret is how ip.c, arp.c and a future forwarding
         * path would each grow their own copy of the same `? dst :` and one of
         * them would get it wrong. */
        out->nexthop = e->nexthop ? e->nexthop : dst;
        out->src = e->src;
        out->flags = e->flags;
        out->metric = e->metric;
        out->plen = e->plen;
    }
    return RT_OK;
}

void route_flush(void)
{
    for (int i = 0; i < RT_NROUTE; i++) tab[i].used = 0;
    for (int i = 0; i < RT_NIF; i++) ifmemo[i].set = 0;
    rt_seq = 0;
    rt_gen++;
}

void route_flush_if(int oif)
{
    if (oif <= 0) return;
    int n = 0;
    for (int i = 0; i < RT_NROUTE; i++)
        if (tab[i].used && tab[i].e.oif == oif) { tab[i].used = 0; n++; }
    if (oif < RT_NIF) ifmemo[oif].set = 0;
    if (n) rt_gen++;
}

const struct route_entry *route_at(int slot)
{
    if (slot < 0 || slot >= RT_NROUTE) return 0;
    return tab[slot].used ? &tab[slot].e : 0;
}

int route_count(void)
{
    int n = 0;
    for (int i = 0; i < RT_NROUTE; i++) if (tab[i].used) n++;
    return n;
}

/* ---- the bridge from an address to the routes it implies ---------------- */

int route_v4_iface(int oif, uint32_t addr, uint32_t mask, uint32_t gw,
                   uint32_t flags)
{
    if (oif <= 0 || oif >= RT_NIF) return RT_EINVAL;
    int plen = route_mask_plen(mask);
    if (plen < 0) return RT_EINVAL;

    struct rt_ifmemo *m = &ifmemo[oif];
    if (m->set && m->addr == addr && m->mask == mask && m->gw == gw &&
        m->flags == flags)
        return RT_OK;                       /* nothing moved; the hot path */

    /* Something moved (a DHCP lease, a settings change, a card coming up).
     * Withdraw this interface's rows before installing the new ones: leaving
     * the old connected route behind would keep sending the previous subnet's
     * traffic out of a card that no longer holds an address in it, and it
     * would keep winning on prefix length while doing so. */
    route_flush_if(oif);

    int rc;
    /* METRIC 0 on both, deliberately. A metric only ever separates routes to
     * the SAME prefix -- the connected route beats the default on prefix
     * length, which is a stronger and earlier rule, so giving the default a
     * worse metric would express nothing and would quietly hide a broken
     * prefix comparison behind a working metric comparison. That is exactly
     * what the negative control needs to be able to see. */
    rc = route_add((struct route_entry){
        .dst = addr & route_plen_mask(plen), .plen = (uint8_t)plen,
        .nexthop = 0, .src = addr, .flags = flags, .oif = oif, .metric = 0,
    });
    if (rc != RT_OK) return rc;

    if (gw) {
        rc = route_add((struct route_entry){
            .dst = 0, .plen = 0, .nexthop = gw, .src = addr,
            .flags = flags, .oif = oif, .metric = 0,
        });
        if (rc != RT_OK) return rc;
    }

    m->addr = addr; m->mask = mask; m->gw = gw; m->flags = flags; m->set = 1;
    return RT_OK;
}
