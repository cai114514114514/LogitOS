#ifndef LOGIT_XHCI_RING_H
#define LOGIT_XHCI_RING_H

/* xHCI Transfer Request Blocks and the three ring flavours built out of them.
 *
 * This file is deliberately free of MMIO, DMA and every other thing that needs
 * a machine: it is pure index arithmetic over an array of 16-byte structures.
 * That is not tidiness for its own sake. The cycle bit is the single hardest
 * thing to get right in an xHCI driver -- producer and consumer agree on who
 * owns a TRB by comparing one bit against a state that toggles every time the
 * ring wraps, and getting the toggle off by one wrap means the controller
 * either ignores everything you enqueue (looks like dead hardware) or executes
 * a ring full of stale TRBs from the last lap (looks like random corruption).
 * Neither failure says which it is from inside QEMU. So the wrap lives here,
 * with tests/unit/usb_ring_test.c driving 10 laps of it on the host.
 *
 * Spec references are to the xHCI 1.2 specification:
 *   6.4    TRB types and field layouts
 *   4.9.2  "Rings" -- the cycle bit / Link TRB / Toggle Cycle protocol
 *   4.11.5 the Event Ring, whose consumer cycle state (CCS) toggles on wrap
 *          because an event ring has NO Link TRB -- the hardware writes the
 *          segment end to end and starts over. That asymmetry with the command
 *          and transfer rings is the other classic bug and is why xring_init
 *          takes an explicit `link` argument instead of guessing.
 */

#include <stdint.h>

/* 6.4.1: every TRB is 16 bytes. `status` and `control` are bitfields whose
 * meaning depends on the TRB type; the accessors below cover what we build. */
struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

/* --- TRB types (6.4.6, Table 6-91) --- */
#define TRB_NORMAL          1
#define TRB_SETUP_STAGE     2
#define TRB_DATA_STAGE      3
#define TRB_STATUS_STAGE    4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIG_EP       12
#define TRB_EVAL_CONTEXT    13
#define TRB_RESET_EP        14
#define TRB_NOOP_CMD        23
#define TRB_TRANSFER_EVENT  32
#define TRB_CMD_COMPLETION  33
#define TRB_PORT_STATUS     34

/* --- control-word bits (6.4.1.1 / 6.4.4.1) --- */
#define TRB_C     (1u << 0)     /* Cycle */
#define TRB_ENT   (1u << 1)     /* Evaluate Next TRB */
#define TRB_ISP   (1u << 2)     /* Interrupt on Short Packet */
#define TRB_NS    (1u << 3)
#define TRB_CH    (1u << 4)     /* Chain */
#define TRB_IOC   (1u << 5)     /* Interrupt On Completion */
#define TRB_IDT   (1u << 6)     /* Immediate Data */
#define TRB_BEI   (1u << 9)
#define TRB_TC    (1u << 1)     /* Toggle Cycle -- Link TRB only (aliases ENT) */
#define TRB_DIR_IN (1u << 16)   /* Data/Status Stage direction */

#define TRB_SET_TYPE(t)  (((uint32_t)(t) & 0x3Fu) << 10)
#define TRB_TYPE(c)      (((c) >> 10) & 0x3Fu)

/* Event TRB decode (6.4.2). `status` carries the completion code in its top
 * byte and the UNTRANSFERRED byte count in the low 24 bits -- residual, not
 * length, which is the field name people get backwards. */
#define TRB_CC(status)       (((status) >> 24) & 0xFFu)
#define TRB_RESIDUAL(status) ((status) & 0xFFFFFFu)
#define TRB_EV_SLOT(ctl)     (((ctl) >> 24) & 0xFFu)
#define TRB_EV_EPID(ctl)     (((ctl) >> 16) & 0x1Fu)   /* == DCI */
#define TRB_EV_PORT(param)   (((param) >> 24) & 0xFFu) /* Port Status Change */

/* Completion codes (6.4.5, Table 6-90) -- the handful we act on. */
#define CC_INVALID           0
#define CC_SUCCESS           1
#define CC_DATA_BUFFER_ERR   2
#define CC_BABBLE            3
#define CC_USB_TRANSACTION   4
#define CC_TRB_ERROR         5
#define CC_STALL             6
#define CC_SHORT_PACKET      13

struct xhci_ring {
    struct trb *trb;      /* segment base; identity-mapped, so virt == phys */
    uint32_t    n;        /* TRBs in the segment (Link TRB included, if any) */
    uint32_t    enq;      /* producer index */
    uint32_t    deq;      /* consumer index (event rings only) */
    uint8_t     cycle;    /* PCS for producers, CCS for event rings */
    uint8_t     has_link; /* 1 = command/transfer ring, 0 = event ring */
    uint32_t    pending;  /* producer-side outstanding TRBs, for xring_space */
};

/* Zero the segment and set up the ring. `link` != 0 installs a Link TRB with
 * Toggle Cycle in the LAST slot pointing back at the base, which is what makes
 * a one-segment ring circular (4.9.2.2). Event rings pass link == 0. */
void xring_init(struct xhci_ring *r, struct trb *base, uint32_t n, int link);

/* Usable slots left before the producer would lap the consumer. */
uint32_t xring_space(const struct xhci_ring *r);

/* Enqueue one TRB. `control` must NOT carry the cycle bit -- this sets it, then
 * advances, crossing the Link TRB (and toggling PCS) when it lands on one.
 * Returns the physical address of the TRB just written, or 0 if full.
 *
 * The cycle bit is written LAST relative to the rest of the TRB by virtue of
 * being part of the same store as the type; callers that need the controller to
 * not see a half-built chain must build the chain with CH set and only then
 * ring the doorbell (which xhci.c does). */
uint64_t xring_push(struct xhci_ring *r, uint64_t param, uint32_t status, uint32_t control);

/* Consumer side of an event ring: copies the next event into *out and advances
 * (toggling CCS on wrap) if the TRB's cycle bit matches. -> 1 if an event was
 * taken, 0 if the ring is empty. */
int xring_pop(struct xhci_ring *r, struct trb *out);

/* Physical address of the current dequeue TRB -- what goes in ERDP. */
uint64_t xring_deq_ptr(const struct xhci_ring *r);
/* Physical address of the base, with the ring's initial cycle in bit 0 -- the
 * form CRCR and the EP context's TR Dequeue Pointer want. */
uint64_t xring_base_dcs(const struct xhci_ring *r);

/* Producer-side accounting: a completion frees one slot. */
void xring_complete(struct xhci_ring *r, uint32_t ntrb);

#endif /* LOGIT_XHCI_RING_H */
