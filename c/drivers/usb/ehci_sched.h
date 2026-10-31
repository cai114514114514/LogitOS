/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_EHCI_SCHED_H
#define LOGIT_EHCI_SCHED_H
#include <stdint.h>
#include <stddef.h>
#include "ehci_hw.h"

/* Pure scheduling arithmetic, using the USB_SPEED_* wire-independent values
 * 1=full, 2=low, 3=high. No allocator, clock, locks or kernel headers here.
 * References throughout are Linux v6.12:
 * https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-sched.c
 * https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-q.c
 * https://github.com/torvalds/linux/blob/v6.12/drivers/usb/core/hcd.c
 * https://github.com/torvalds/linux/blob/v6.12/include/linux/usb/hcd.h
 * This is model-tested; not verified on X79-VG4 hardware. */
#define EHCI_PERIODIC_FRAMES 1024u /* EHCI 2.3.4 default frame-list size. */
#define EHCI_BW_UFRAMES 64u       /* Linux EHCI_BANDWIDTH_SIZE. */
#define EHCI_BW_FRAMES 8u
#define EHCI_UFRAME_PERIODIC_MAX_US 100u /* ehci-hcd.c: 80% of 125 us. */
#define EHCI_PHASE_NONE 0xffffu
#define EHCI_TT_MAX 8

#ifndef EHCI_HW_STORE
#define EHCI_HW_STORE(ptr, value) (*(ptr) = (value))
#endif

struct ehci_ps {
    uint8_t speed, is_in, phase_uf, smask, cmask, gap_uf, us_s, us_c;
    uint16_t period, uperiod_bw, phase, bw_phase, us_tt;
    uint8_t hs_mask;
};
struct ehci_tt_acct {
    uint16_t hub_addr;
    uint8_t port, used;
    uint16_t frame_us[EHCI_BW_FRAMES];
    uint8_t n;
    /* The draft interface had 32 pointers, but its 48-endpoint capacity gate
     * needs 48 distinct contributions to recompute carry and release exactly.
     * 64 bounds this pure helper; the production HC still has 32 endpoints. */
    struct ehci_ps *ps[64];
};
struct ehci_sched {
    uint8_t bw_us[EHCI_BW_UFRAMES];
    uint32_t rotor;
    struct ehci_tt_acct tt[EHCI_TT_MAX];
};

/* USB 2.0 5.11.3, Linux hcd.h HS_NSECS: preserve BitTime's integer
 * truncation, add USB2_HOST_DELAY=5 ns, then round UP once to microseconds. */
static inline unsigned ehci_hs_us(unsigned bytes)
{
    unsigned bit_time = 7u * 8u * bytes / 6u;
    unsigned ns = (55u * 8u * 2083u + 2083u * (3u + bit_time)) / 1000u + 5u;
    return (ns + 999u) / 1000u;
}

/* Non-isochronous usb_calc_bus_time(), Linux hcd.c. BW_HOST_DELAY=1000 ns
 * and BW_HUB_LS_SETUP=333 ns are included, even for an empty packet. */
static inline unsigned ehci_bus_ns(unsigned speed, int is_in, unsigned bytes)
{
    unsigned bit_time = 7u * 8u * bytes / 6u;
    if (speed == 2) {
        unsigned scaled = (is_in ? 67667u : 66700u) * (31u + 10u * bit_time) / 1000u;
        return scaled + (is_in ? 64060u : 64107u) + 2u * 333u + 1000u;
    }
    if (speed == 1) {
        return 8354u * (31u + 10u * bit_time) / 1000u + 9107u + 1000u;
    }
    if (speed == 3) {
        return (55u * 8u * 2083u + 2083u * (3u + bit_time)) / 1000u + 5u;
    }
    return 0;
}

/* Start with no bandwidth and deterministic rotor zero, so boot diagnostics
 * can be compared without Linux's random_frame seed hiding a phase change. */
static inline void ehci_sched_init(struct ehci_sched *sched)
{
    *sched = (struct ehci_sched){0};
}

/* find_tt(): a single translator is shared by every downstream port; a
 * multi-TT hub owns one translator per port. QH routing still uses the real
 * port in both cases (ehci-q.c qh_make), independently of this key. */
static inline struct ehci_tt_acct *ehci_sched_tt(struct ehci_sched *sched,
        unsigned hub_addr, unsigned port, int multi)
{
    if (!hub_addr || hub_addr > 127 || !port || port > 127) {
        return NULL;
    }
    unsigned key_port = multi ? port : 0;
#ifdef EHCI_NEGCTL_SHARED_TT_KEY
    key_port = port;
#endif
    struct ehci_tt_acct *empty = NULL;
    for (unsigned i = 0; i < EHCI_TT_MAX; i++) {
        struct ehci_tt_acct *tt = &sched->tt[i];
        if (tt->used && tt->hub_addr == hub_addr && tt->port == key_port) {
            return tt;
        }
        if (!tt->used && !empty) {
            empty = tt;
        }
    }
    if (empty) {
        *empty = (struct ehci_tt_acct){.hub_addr = hub_addr, .port = key_port, .used = 1};
    }
    return empty;
}

/* qh_make(): derive immutable costs before picking a slot. Splits use Linux's
 * approximate HS SS/CS costs and exact rounded downstream bus time + think.
 * FSTNs, isochronous and high-bandwidth multiplier packets remain absent. */
static inline int ehci_ps_make(struct ehci_ps *ps, unsigned speed, int is_in,
        unsigned max_packet, unsigned binterval, unsigned think_ns)
{
    unsigned mask;
    int period = ehci_period(speed, binterval, &mask);
    if (!ps || period < 1 || !max_packet || max_packet > 1024 ||
        (speed == 2 && max_packet > 8) || (speed == 1 && max_packet > 64)) {
        return -1;
    }
    *ps = (struct ehci_ps){.speed = speed, .is_in = !!is_in, .period = period,
        .phase = EHCI_PHASE_NONE, .bw_phase = EHCI_PHASE_NONE, .smask = mask};
    ps->us_s = ehci_hs_us(max_packet);
    if (speed == 3) {
        unsigned uperiod = 1u << (binterval - 1);
        ps->uperiod_bw = uperiod < EHCI_BW_UFRAMES ? uperiod : EHCI_BW_UFRAMES;
        ps->hs_mask = binterval <= 3 ? mask : 0;
    } else {
        unsigned ns = ehci_bus_ns(speed, is_in, max_packet);
        ps->gap_uf = 1u + ns / 125000u;
        ps->us_tt = (ns + think_ns + 999u) / 1000u;
        ps->us_c = is_in ? ps->us_s + ehci_hs_us(0) : ehci_hs_us(0);
        ps->us_s = is_in ? ehci_hs_us(1) : ps->us_s + ehci_hs_us(1);
        ps->uperiod_bw = (period < 8 ? period : 8) * 8;
    }
    return 0;
}

/* check_period(): test every occurrence in the 8-frame accounting window.
 * Refuse cross-frame microframes rather than silently wrapping a CSPLIT. */
static inline int ehci_check_period(const struct ehci_sched *sched,
        unsigned frame, unsigned uframe, unsigned uperiod, unsigned usecs)
{
    if (uframe >= 8 || !uperiod || usecs > EHCI_UFRAME_PERIODIC_MAX_US) {
        return 0;
    }
#ifdef EHCI_NEGCTL_NO_BW_LIMIT
    return 1;
#endif
    for (unsigned slot = frame * 8 + uframe; slot < EHCI_BW_UFRAMES; slot += uperiod) {
        if (sched->bw_us[slot] > EHCI_UFRAME_PERIODIC_MAX_US - usecs) {
            return 0;
        }
    }
    return 1;
}

/* compute_tt_budget(): downstream transactions can spill into later
 * microframes. Sum in start order to avoid making admission depend on list
 * insertion order when a newly admitted transfer starts before an old one. */
static inline void ehci_tt_budget(const struct ehci_tt_acct *tt, unsigned frame,
        unsigned budget[8])
{
    for (unsigned uf = 0; uf < 8; uf++) {
        budget[uf] = 0;
        for (unsigned i = 0; i < tt->n; i++) {
            const struct ehci_ps *ps = tt->ps[i];
            unsigned period = ps->uperiod_bw / 8;
            if (ps->phase_uf == uf && frame % period == ps->bw_phase) {
                budget[uf] += ps->us_tt;
            }
        }
    }
    for (unsigned uf = 0; uf < 7; uf++) {
        if (budget[uf] > 125) {
            budget[uf + 1] += budget[uf] - 125;
            budget[uf] = 125;
        }
    }
}

/* tt_available() with CONFIG_USB_EHCI_TT_NEWSCHED: USB2 11.18.1 Fig 11-60
 * allows 125 us in B-uframes 0..5, only 30 in 6, none in 7, and 900 us/frame.
 * Also enforce USB2 11.18.4's 16 SSPLITs per uframe explicitly: tiny packets
 * can hit that bound before the downstream-time budget fills. */
static inline int ehci_tt_available(const struct ehci_tt_acct *tt,
        const struct ehci_ps *ps, unsigned frame, unsigned uframe)
{
    if (!tt || uframe >= 6 || ps->uperiod_bw < 8) {
        return 0;
    }
#ifdef EHCI_NEGCTL_NO_TT_BUDGET
    return 1;
#endif
    unsigned period = ps->uperiod_bw / 8;
    for (frame &= period - 1; frame < EHCI_BW_FRAMES; frame += period) {
        unsigned budget[8];
        ehci_tt_budget(tt, frame, budget);
        if (tt->frame_us[frame] + ps->us_tt > 900 || budget[uframe] >= 125) {
            return 0;
        }
        unsigned starts = 0;
        for (unsigned i = 0; i < tt->n; i++) {
            const struct ehci_ps *other = tt->ps[i];
            if (other->phase_uf == uframe && frame % (other->uperiod_bw / 8) == other->bw_phase) {
                starts++;
            }
        }
        if (starts >= 16) {
            return 0;
        }
        budget[uframe] += ps->us_tt;
        for (unsigned uf = 0; uf < 7; uf++) {
            unsigned limit = uf == 6 ? 30 : 125;
            if (budget[uf] > limit) {
                budget[uf + 1] += budget[uf] - limit;
                budget[uf] = limit;
            }
        }
        if (budget[7]) {
            return 0;
        }
    }
    return 1;
}

/* check_intr_schedule(): SSPLIT in H-uframe u, CSPLIT in u+2..u+4 (up to
 * uframe 7); no FSTN wrap. Subframe HS endpoints check their whole HS mask. */
static inline int ehci_check_intr(const struct ehci_sched *sched,
        const struct ehci_tt_acct *tt, const struct ehci_ps *ps,
        unsigned frame, unsigned uframe, unsigned *cmask)
{
    *cmask = 0;
    if (ps->us_c && uframe >= 6) {
        return 0;
    }
    if (!ehci_check_period(sched, frame, uframe, ps->uperiod_bw, ps->us_s)) {
        return 0;
    }
    if (!ps->us_c) {
        return 1;
    }
    if (!ehci_tt_available(tt, ps, frame, uframe)) {
        return 0;
    }
    for (unsigned uf = uframe + 2; uf < 8 && uf <= uframe + 4; uf++) {
        if (!ehci_check_period(sched, frame, uf, ps->uperiod_bw, ps->us_c)) {
            return 0;
        }
        *cmask |= 1u << uf;
    }
#ifdef EHCI_NEGCTL_CMASK_SAME_AS_SMASK
    *cmask = 1u << uframe;
#endif
    return 1;
}

/* reserve_release_intr_bandwidth(): one accounting routine for both signs
 * keeps release exactly inverse, including all three CS opportunities. */
static inline void ehci_sched_charge(struct ehci_sched *sched,
        struct ehci_tt_acct *tt, struct ehci_ps *ps, int sign)
{
    for (unsigned slot = ps->bw_phase * 8 + ps->phase_uf;
         slot < EHCI_BW_UFRAMES; slot += ps->uperiod_bw) {
        sched->bw_us[slot] += sign * ps->us_s;
    }
    if (ps->us_c) {
        for (unsigned frame = ps->bw_phase; frame < EHCI_BW_FRAMES;
             frame += ps->uperiod_bw / 8) {
            for (unsigned uf = 0; uf < 8; uf++) {
                if (ps->cmask & (1u << uf)) {
                    sched->bw_us[frame * 8 + uf] += sign * ps->us_c;
                }
            }
            tt->frame_us[frame] += sign * ps->us_tt;
        }
    }
}

/* qh_schedule(): deterministic first fit, increment rotor before taking the
 * phase. Keep long hardware periods while conservatively repeating their
 * reservations in the smaller Linux bandwidth window. */
static inline int ehci_sched_admit(struct ehci_sched *sched,
        struct ehci_tt_acct *tt, struct ehci_ps *ps)
{
    if (ps->phase != EHCI_PHASE_NONE) {
        return 0;
    }
    if (ps->us_c && (!tt || tt->n == sizeof(tt->ps) / sizeof(tt->ps[0]))) {
        return -1;
    }
    unsigned bw_frames = ps->uperiod_bw >= 8 ? ps->uperiod_bw / 8 : 1;
    for (unsigned attempt = 0; attempt < bw_frames; attempt++) {
        unsigned phase = ++sched->rotor & (ps->period - 1);
        unsigned bw_phase = phase & (bw_frames - 1);
        unsigned uf_count = ps->uperiod_bw < 8 ? ps->uperiod_bw : 8;
        for (unsigned uf = 0; uf < uf_count; uf++) {
            unsigned cmask;
            if (!ehci_check_intr(sched, tt, ps, bw_phase, uf, &cmask)) {
                continue;
            }
            ps->phase = phase;
            ps->bw_phase = bw_phase;
            ps->phase_uf = uf;
            ps->smask = ps->hs_mask ? ps->hs_mask << uf : 1u << uf;
            ps->cmask = cmask;
            if (ps->us_c) {
                tt->ps[tt->n++] = ps;
            }
            ehci_sched_charge(sched, tt, ps, 1);
            return 0;
        }
    }
    return -1;
}

/* Return bandwidth only after the HC grace period; a still-prefetched QH
 * must not compete with an endpoint reusing its budget (EHCI 4.6/4.12). */
static inline void ehci_sched_release(struct ehci_sched *sched,
        struct ehci_tt_acct *tt, struct ehci_ps *ps)
{
    if (ps->phase == EHCI_PHASE_NONE) {
        /* Linux drop_tt(): a lookup followed by failed admission must not
         * consume one of the bounded TT keys forever. */
        if (tt && !tt->n) {
            *tt = (struct ehci_tt_acct){0};
        }
        return;
    }
    ehci_sched_charge(sched, tt, ps, -1);
    if (tt) {
        for (unsigned i = 0; i < tt->n; i++) {
            if (tt->ps[i] == ps) {
                for (unsigned j = i + 1; j < tt->n; j++) {
                    tt->ps[j - 1] = tt->ps[j];
                }
                tt->ps[--tt->n] = NULL;
                break;
            }
        }
        if (!tt->n) {
            *tt = (struct ehci_tt_acct){0};
        }
    }
    ps->phase = ps->bw_phase = EHCI_PHASE_NONE;
}

struct ehci_pnode {
    volatile uint32_t *hw_next;
    uint32_t dma;
    struct ehci_pnode *next;
    uint16_t period, phase;
    uint8_t linked;
};
struct ehci_ptree {
    volatile uint32_t *list;
    struct ehci_pnode *shadow[EHCI_PERIODIC_FRAMES];
};

/* EHCI 4.6: initialize all terminate bits before publishing PERIODICLISTBASE. */
static inline void ehci_ptree_init(struct ehci_ptree *tree, volatile uint32_t *list)
{
    tree->list = list;
    for (unsigned frame = 0; frame < EHCI_PERIODIC_FRAMES; frame++) {
        tree->shadow[frame] = NULL;
        EHCI_HW_STORE(&list[frame], EHCI_END);
    }
}

/* Linux qh_link_periodic(): descending powers of two produce shared tails.
 * Initialize the new successor before any predecessor becomes HC-visible.
 * The release fence is a DMA write barrier on this x86 coherent backend;
 * callers also dma_wmb() before entering here to publish the whole QH. */
static inline void ehci_ptree_link(struct ehci_ptree *tree, struct ehci_pnode *node)
{
    if (node->linked) {
        return;
    }
    int initialized = 0;
    for (unsigned frame = node->phase; frame < EHCI_PERIODIC_FRAMES; frame += node->period) {
        struct ehci_pnode **shadow = &tree->shadow[frame];
        volatile uint32_t *hardware = &tree->list[frame];
        while (*shadow && *shadow != node && (*shadow)->period >= node->period) {
#ifdef EHCI_NEGCTL_TREE_FAST_FIRST
            break;
#endif
            hardware = (*shadow)->hw_next;
            shadow = &(*shadow)->next;
        }
        if (*shadow == node) {
            continue;
        }
        if (!initialized) {
            node->next = *shadow;
#ifdef EHCI_NEGCTL_LINK_BEFORE_INIT
            EHCI_HW_STORE(hardware, node->dma | EHCI_QH_LINK);
#endif
            EHCI_HW_STORE(node->hw_next, *hardware);
            initialized = 1;
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        *shadow = node;
        EHCI_HW_STORE(hardware, node->dma | EHCI_QH_LINK);
    }
    node->linked = 1;
}

/* Linux periodic_unlink(): patch every incoming pointer, but leave the removed
 * node's own next intact until 9 microframes have elapsed (ehci-timer.c).
 * Shared predecessors may already have been patched by an earlier frame. */
static inline void ehci_ptree_unlink(struct ehci_ptree *tree, struct ehci_pnode *node)
{
    if (!node->linked) {
        return;
    }
    for (unsigned frame = node->phase; frame < EHCI_PERIODIC_FRAMES; frame += node->period) {
        struct ehci_pnode **shadow = &tree->shadow[frame];
        volatile uint32_t *hardware = &tree->list[frame];
        while (*shadow && *shadow != node) {
            hardware = (*shadow)->hw_next;
            shadow = &(*shadow)->next;
        }
        if (*shadow == node) {
            *shadow = node->next;
            EHCI_HW_STORE(hardware, *node->hw_next);
        }
    }
    node->linked = 0;
}
#endif
