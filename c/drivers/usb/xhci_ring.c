#include "xhci_ring.h"

/* No libc here: this file is compiled both into the kernel (which has its own
 * memset in c/lib/string.c) and into a host unit test. A four-line local zero
 * loop is cheaper than arguing about which memset each build gets. */
static void zero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

void xring_init(struct xhci_ring *r, struct trb *base, uint32_t n, int link)
{
    r->trb = base;
    r->n = n;
    r->enq = 0;
    r->deq = 0;
    r->cycle = 1;            /* 4.9.2: both sides start at 1 after a reset */
    r->has_link = link ? 1 : 0;
    r->pending = 0;
    zero(base, (unsigned long)n * sizeof *base);

    if (link) {
        /* 4.9.2.2: the last TRB of the only segment links back to the start and
         * asks the controller to flip ITS cycle state there, so one segment
         * behaves like an infinite ring. Its own cycle bit is left 0 and is
         * written by xring_push when the producer reaches it -- publishing the
         * link only once the TRB before it is complete. */
        struct trb *l = &base[n - 1];
        l->param = (uint64_t)(uintptr_t)base;
        l->status = 0;
        l->control = TRB_SET_TYPE(TRB_LINK) | TRB_TC;
    }
}

uint32_t xring_space(const struct xhci_ring *r)
{
    uint32_t usable = r->has_link ? r->n - 1 : r->n;
    return r->pending >= usable ? 0 : usable - r->pending;
}

uint64_t xring_push(struct xhci_ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    if (!xring_space(r)) return 0;

    struct trb *t = &r->trb[r->enq];
    t->param = param;
    t->status = status;
    t->control = (control & ~TRB_C) | (r->cycle ? TRB_C : 0);
    uint64_t phys = (uint64_t)(uintptr_t)t;

    r->enq++;
    r->pending++;

    /* Landed on the Link TRB: hand it to the controller with the CURRENT cycle
     * (so it is "owned" like the TRBs before it), then follow it -- which means
     * wrapping to 0 and, because the link carries Toggle Cycle, flipping PCS.
     * Doing the toggle without publishing the link, or publishing it with the
     * new cycle, are the two ways to make the controller stop dead here. */
    if (r->has_link && r->enq == r->n - 1) {
        struct trb *l = &r->trb[r->n - 1];
        l->control = (l->control & ~TRB_C) | (r->cycle ? TRB_C : 0);
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

int xring_pop(struct xhci_ring *r, struct trb *out)
{
    struct trb *t = &r->trb[r->deq];
    uint8_t c = (t->control & TRB_C) ? 1 : 0;
    if (c != r->cycle) return 0;         /* the controller has not written it yet */

    *out = *t;
    r->deq++;
    if (r->deq == r->n) {                /* 4.11.5: no Link TRB, so wrap by hand */
        r->deq = 0;
        r->cycle ^= 1;
    }
    return 1;
}

uint64_t xring_deq_ptr(const struct xhci_ring *r)
{
    return (uint64_t)(uintptr_t)&r->trb[r->deq];
}

uint64_t xring_base_dcs(const struct xhci_ring *r)
{
    return (uint64_t)(uintptr_t)r->trb | (r->cycle ? 1u : 0u);
}

void xring_complete(struct xhci_ring *r, uint32_t ntrb)
{
    r->pending = r->pending > ntrb ? r->pending - ntrb : 0;
}
