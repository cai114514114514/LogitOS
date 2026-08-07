/* Host unit test for c/drivers/usb/xhci_ring.c -- the cycle bit, the Link TRB,
 * and the two rings that disagree about how wrapping works.
 *
 * The claim under test is not "push writes a TRB". It is that after N laps of a
 * ring the producer and the controller still agree on who owns each slot. That
 * agreement is one bit wide and toggles on wrap, and every way of getting it
 * wrong looks, from inside QEMU, like hardware that stopped responding. So this
 * drives a deliberately tiny ring (8 TRBs) for many laps and checks the cycle
 * bit of every TRB written, plus the Link TRB's own cycle, which is the one
 * everybody forgets to publish.
 *
 * Build (host, no QEMU):
 *   cc -O2 -Wall -Wextra -o build/usb_ring_test tests/unit/usb_ring_test.c \
 *      c/drivers/usb/xhci_ring.c -Ic/drivers/usb && ./build/usb_ring_test
 */

#include <stdio.h>
#include <string.h>
#include "xhci_ring.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

static void checki(long long got, long long want, const char *what)
{
    if (got != want) { printf("  FAIL %s: got %lld, want %lld\n", what, got, want); failures++; }
}

#define N 8
static struct trb seg[N];
static struct trb evseg[N];

int main(void)
{
    struct xhci_ring r;

    /* --- init installs a Link TRB with Toggle Cycle, pointing home --- */
    xring_init(&r, seg, N, 1);
    checki(TRB_TYPE(seg[N - 1].control), TRB_LINK, "last TRB is a Link TRB");
    check((seg[N - 1].control & TRB_TC) != 0, "the Link TRB carries Toggle Cycle");
    check(seg[N - 1].param == (uint64_t)(uintptr_t)seg, "the Link TRB points at the segment base");
    checki(seg[N - 1].control & TRB_C, 0, "the Link TRB is not published before the producer reaches it");
    checki(r.cycle, 1, "PCS starts at 1");
    checki(xring_space(&r), N - 1, "a link ring has n-1 usable slots");

    /* An event ring has NO link TRB: the controller writes the segment end to
     * end. Installing one would make the controller overwrite our link. */
    struct xhci_ring er;
    xring_init(&er, evseg, N, 0);
    checki(TRB_TYPE(evseg[N - 1].control), 0, "an event ring gets no Link TRB");
    checki(xring_space(&er), N, "an event ring has all n slots");

    /* --- one lap: every TRB published with the current PCS --- */
    xring_init(&r, seg, N, 1);
    for (int i = 0; i < N - 1; i++) {
        uint64_t p = xring_push(&r, 0x1000 + i, i, TRB_SET_TYPE(TRB_NORMAL));
        checki(p, (long long)(uintptr_t)&seg[i], "push returns the physical address of the slot it wrote");
        checki(seg[i].control & TRB_C, TRB_C, "TRB published with PCS=1 on lap 0");
        checki(seg[i].param, 0x1000 + i, "param stored");
        checki(TRB_TYPE(seg[i].control), TRB_NORMAL, "type stored");
    }
    /* The (N-1)th push lands on the Link TRB, publishes it, wraps and toggles. */
    checki(seg[N - 1].control & TRB_C, TRB_C, "the Link TRB is published with the OLD cycle");
    checki(r.cycle, 0, "PCS toggled through the Link TRB");
    checki(r.enq, 0, "enqueue wrapped to the segment base");

    /* --- full ring refuses rather than lapping the consumer --- */
    checki(xring_space(&r), 0, "ring is full with n-1 outstanding");
    checki(xring_push(&r, 0xdead, 0, TRB_SET_TYPE(TRB_NORMAL)), 0, "push on a full ring is refused");
    checki(seg[0].param, 0x1000, "a refused push did not overwrite slot 0");

    /* --- many laps: the cycle bit alternates per lap, forever --- */
    xring_init(&r, seg, N, 1);
    for (int lap = 0; lap < 10; lap++) {
        int want_c = (lap % 2 == 0) ? TRB_C : 0;
        for (int i = 0; i < N - 1; i++) {
            check(xring_push(&r, lap * 100 + i, 0, TRB_SET_TYPE(TRB_NORMAL)) != 0, "push during lap");
            checki(seg[i].control & TRB_C, want_c, "TRB carries the lap's cycle state");
        }
        checki(seg[N - 1].control & TRB_C, want_c, "the Link TRB carries the lap's cycle state");
        xring_complete(&r, N - 1);          /* the controller finished the lap */
        checki(xring_space(&r), N - 1, "completions free the slots again");
    }
    checki(r.cycle, 1, "after 10 laps PCS is back where it started");

    /* --- the cycle bit is set by the ring, not the caller --- */
    xring_init(&r, seg, N, 1);
    xring_push(&r, 0, 0, TRB_SET_TYPE(TRB_NORMAL) | TRB_C);   /* caller wrongly sets C */
    checki(seg[0].control & TRB_C, TRB_C, "a caller-set cycle bit is harmless on lap 0");
    xring_init(&r, seg, N, 1);
    r.cycle = 0;                                               /* pretend lap 1 */
    xring_push(&r, 0, 0, TRB_SET_TYPE(TRB_NORMAL) | TRB_C);
    checki(seg[0].control & TRB_C, 0, "the ring OVERRIDES a caller-set cycle bit");

    /* --- event ring consumption: cycle match, wrap, CCS toggle --- */
    xring_init(&er, evseg, N, 0);
    struct trb e;
    checki(xring_pop(&er, &e), 0, "an untouched event ring is empty (CCS=1, TRBs are 0)");

    /* The controller writes events with ITS cycle state. Simulate two full laps. */
    for (int lap = 0; lap < 4; lap++) {
        int hw_c = (lap % 2 == 0) ? 1 : 0;
        for (int i = 0; i < N; i++) {
            evseg[i].param = lap * 1000 + i;
            evseg[i].status = 0;
            evseg[i].control = TRB_SET_TYPE(TRB_TRANSFER_EVENT) | (hw_c ? TRB_C : 0);
        }
        for (int i = 0; i < N; i++) {
            checki(xring_pop(&er, &e), 1, "event popped");
            checki(e.param, lap * 1000 + i, "events come out in order");
        }
        checki(xring_pop(&er, &e), 0, "ring empty until the controller writes the next lap");
        checki(er.deq, 0, "event dequeue wrapped to 0");
    }
    checki(er.cycle, 1, "CCS is back to 1 after an even number of laps");

    /* --- ERDP / CRCR pointer forms --- */
    xring_init(&er, evseg, N, 0);
    checki(xring_deq_ptr(&er), (long long)(uintptr_t)&evseg[0], "ERDP points at the current dequeue TRB");
    evseg[0].control = TRB_SET_TYPE(TRB_PORT_STATUS) | TRB_C;
    xring_pop(&er, &e);
    checki(xring_deq_ptr(&er), (long long)(uintptr_t)&evseg[1], "ERDP advances with the consumer");

    xring_init(&r, seg, N, 1);
    checki(xring_base_dcs(&r), (long long)((uintptr_t)seg | 1), "CRCR carries RCS in bit 0");

    /* --- event decode accessors: residual is UNtransferred bytes --- */
    uint32_t st = (CC_SHORT_PACKET << 24) | 5;
    checki(TRB_CC(st), CC_SHORT_PACKET, "completion code out of the status word");
    checki(TRB_RESIDUAL(st), 5, "residual out of the status word");
    uint32_t ctl = TRB_SET_TYPE(TRB_TRANSFER_EVENT) | (3u << 24) | (7u << 16);
    checki(TRB_EV_SLOT(ctl), 3, "slot id out of an event control word");
    checki(TRB_EV_EPID(ctl), 7, "endpoint id (DCI) out of an event control word");
    checki(TRB_TYPE(ctl), TRB_TRANSFER_EVENT, "type out of an event control word");

    if (failures) { printf("usb_ring_test: %d FAILURE(S)\n", failures); return 1; }
    printf("usb_ring_test: ok (link TRB, cycle toggle over 10 laps, full-ring refusal, "
           "event-ring wrap without a link, ERDP/CRCR forms, event decode)\n");
    return 0;
}
