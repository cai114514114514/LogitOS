#ifndef LOGIT_XHCI_H
#define LOGIT_XHCI_H

/* eXtensible Host Controller Interface -- the register map and the controller
 * object. Implementation in xhci.c.
 *
 * WHY xHCI AND NOTHING ELSE. USB has had four host controller interfaces: UHCI
 * and OHCI (USB 1.1), EHCI (2.0) and xHCI (3.x). They are not layers, they are
 * alternatives -- except that xHCI is the one that speaks to low, full, high AND
 * SuperSpeed devices itself, through the same rings, with the speed as a field
 * in a context. A machine with xHCI needs no other host controller driver, and a
 * machine without xHCI has not shipped in about a decade. So this is the only
 * one worth writing, and the absence of EHCI/OHCI/UHCI here is a decision, not a
 * gap.
 *
 * Section numbers below are xHCI 1.2.
 */

#include <stdint.h>
#include "xhci_ring.h"
#include "usb_desc.h"

/* --- Host Controller Capability Registers (5.3) --- */
#define XCAP_CAPLENGTH   0x00   /* u8: bytes from cap base to the operational regs */
#define XCAP_HCIVERSION  0x02   /* u16 */
#define XCAP_HCSPARAMS1  0x04
#define XCAP_HCSPARAMS2  0x08
#define XCAP_HCSPARAMS3  0x0C
#define XCAP_HCCPARAMS1  0x10
#define XCAP_DBOFF       0x14
#define XCAP_RTSOFF      0x18

#define HCS1_MAXSLOTS(v) ((v) & 0xFF)
#define HCS1_MAXINTRS(v) (((v) >> 8) & 0x7FF)
#define HCS1_MAXPORTS(v) (((v) >> 24) & 0xFF)
/* 5.3.4: the scratchpad buffer count is split across two fields, high bits at
 * 25:21 and low bits at 31:27 -- reading only the low field yields 0 on any
 * controller that wants more than 31 pages, and the controller then DMAs to a
 * null pointer the moment it is started. */
#define HCS2_SPB(v)      ((((v) >> 27) & 0x1F) | ((((v) >> 21) & 0x1F) << 5))
#define HCC1_AC64(v)     ((v) & 1)
#define HCC1_CSZ(v)      (((v) >> 2) & 1)     /* 1 = 64-byte contexts */
#define HCC1_XECP(v)     (((v) >> 16) & 0xFFFF)  /* offset in DWORDS from cap base */

/* --- Host Controller Operational Registers (5.4) --- */
#define XOP_USBCMD   0x00
#define XOP_USBSTS   0x04
#define XOP_PAGESIZE 0x08
#define XOP_DNCTRL   0x14
#define XOP_CRCR     0x18   /* 64-bit */
#define XOP_DCBAAP   0x30   /* 64-bit */
#define XOP_CONFIG   0x38
#define XOP_PORTSC(n) (0x400 + ((n) - 1) * 0x10)   /* n is 1-based (5.4.8) */

#define CMD_RS    (1u << 0)   /* Run/Stop */
#define CMD_HCRST (1u << 1)   /* Host Controller Reset */
#define CMD_INTE  (1u << 2)   /* Interrupter Enable */
#define CMD_HSEE  (1u << 3)

#define STS_HCH   (1u << 0)   /* HCHalted */
#define STS_HSE   (1u << 2)   /* Host System Error */
#define STS_EINT  (1u << 3)
#define STS_PCD   (1u << 4)   /* Port Change Detect */
#define STS_CNR   (1u << 11)  /* Controller Not Ready */

/* --- PORTSC (5.4.8) --- */
#define PORTSC_CCS  (1u << 0)    /* Current Connect Status */
#define PORTSC_PED  (1u << 1)    /* Port Enabled -- RW1CS, so never write it back */
#define PORTSC_OCA  (1u << 3)
#define PORTSC_PR   (1u << 4)    /* Port Reset */
#define PORTSC_PP   (1u << 9)    /* Port Power */
#define PORTSC_CSC  (1u << 17)
#define PORTSC_PEC  (1u << 18)
#define PORTSC_WRC  (1u << 19)
#define PORTSC_OCC  (1u << 20)
#define PORTSC_PRC  (1u << 21)   /* Port Reset Change */
#define PORTSC_PLC  (1u << 22)
#define PORTSC_CEC  (1u << 23)
#define PORTSC_WPR  (1u << 31)   /* Warm Port Reset (USB3 only) */
#define PORTSC_RW1C (0x7Fu << 17)
#define PORTSC_PLS(v)   (((v) >> 5) & 0xF)
#define PORTSC_SPEED(v) (((v) >> 10) & 0xF)

/* Port Speed IDs (7.2.2.1.1). These are also the Slot Context Speed encoding,
 * which is the only reason a driver can copy one into the other. */
#define XSPEED_FULL  1
#define XSPEED_LOW   2
#define XSPEED_HIGH  3
#define XSPEED_SUPER 4

/* --- Interrupter Register Set (5.5.2), at runtime base + 0x20 + 32*n --- */
#define XRT_MFINDEX 0x00
#define XRT_IR0     0x20
#define XIR_IMAN    0x00
#define XIR_IMOD    0x04
#define XIR_ERSTSZ  0x08
#define XIR_ERSTBA  0x10    /* 64-bit */
#define XIR_ERDP    0x18    /* 64-bit */
#define ERDP_EHB    (1u << 3)   /* Event Handler Busy -- RW1C */

/* --- Endpoint types (6.2.3, Table 6-9) --- */
#define EP_TYPE_ISOCH_OUT 1
#define EP_TYPE_BULK_OUT  2
#define EP_TYPE_INT_OUT   3
#define EP_TYPE_CONTROL   4
#define EP_TYPE_ISOCH_IN  5
#define EP_TYPE_BULK_IN   6
#define EP_TYPE_INT_IN    7

/* Device Context Index: EP0 is 1, EP n IN is 2n+1, EP n OUT is 2n (4.5.1). */
#define XHCI_DCI(addr) (USB_EP_NUM(addr) * 2 + (USB_EP_IS_IN(addr) ? 1 : 0))

/* Control transfer TRT field (6.4.1.2.1) */
#define TRT_NO_DATA 0
#define TRT_OUT     2
#define TRT_IN      3

#define XHCI_MAX_SLOTS 8       /* devices we will address at once */
#define XHCI_EVQ       8       /* per-endpoint completion FIFO depth */
#define XHCI_RING_TRBS 64      /* TRBs per ring segment: 64*16 = 1 KiB, and one
                                * page holds four. Well under the 64 KiB boundary
                                * a ring segment may not cross (6.4.4.1). */

struct usb_device;
struct device;          /* c/drivers/core/driver.h -- the PCI function we bound */

/* One endpoint's transfer ring plus the completions the event dispatcher has
 * parked for whoever is waiting on it. The FIFO exists because a single control
 * transfer can produce TWO events (a Short Packet event for the data stage and a
 * Success event for the status stage) and the residual we need lives in the
 * first one -- a "remember the last event" design silently loses the length of
 * every short descriptor read, which is most of them. */
struct xhci_ep {
    uint8_t  dci;
    uint8_t  used;
    struct xhci_ring ring;
    struct trb ev[XHCI_EVQ];
    volatile uint8_t ev_head, ev_tail;
    volatile uint8_t inflight;      /* an interrupt-IN Normal TRB is queued */
    uint8_t *buf;                   /* DMA buffer for interrupt IN */
    int      buflen;
};

struct xhci_slot {
    uint8_t used;
    struct usb_device *dev;
    uint8_t *dev_ctx;               /* Device Context (4.5) */
    uint8_t *in_ctx;                /* Input Context (6.2.5) */
    struct xhci_ep *ep[32];
};

struct xhci {
    volatile uint8_t  *cap;
    volatile uint8_t  *op;
    volatile uint8_t  *rt;
    volatile uint32_t *db;
    int maxslots, maxports, ctxsize;
    uint64_t *dcbaa;
    uint64_t *scratch_arr;
    struct xhci_ring cmd;
    struct xhci_ring ev;
    uint8_t *erst;
    struct xhci_slot slot[XHCI_MAX_SLOTS + 1];   /* slot ids are 1-based */

    /* Command completion rendezvous: one command is outstanding at a time,
     * which is all this driver ever needs and makes the wait a single compare
     * against the TRB address the controller echoes back (6.4.2.2). */
    volatile uint64_t cmd_want;
    volatile int cmd_ready, cmd_cc, cmd_slot;

    /* Counters the boot test reads back over serial. A driver that reports only
     * "up" cannot be distinguished from one that enumerated nothing. */
    unsigned long events, transfers, port_changes, errors;
    int up;
};

extern struct xhci g_xhci;

/* Bring the controller up: reset, DCBAA, command ring, event ring + ERST, run.
 * `dev` is the PCI function the device model bound by class. -> 0 on success.
 * Does NOT enumerate; usb_core.c drives the ports. */
int  xhci_init(struct device *dev);

/* Drain the event ring into the per-endpoint FIFOs and the command rendezvous,
 * acknowledging the interrupt first. This IS the interrupt handler's body;
 * bounded, allocation-free, and safe to call from interrupt context. */
void xhci_events(void);

/* Arm the interrupter, once a CPU vector is behind it. */
void xhci_irq_enable(void);

/* Root hub. `speed` is a XSPEED_* value. */
int  xhci_port_count(void);
int  xhci_port_connected(int port);
int  xhci_port_reset(int port, int *speed);

/* Device setup, in the order 4.3 "USB Device Initialization" prescribes. */
int  xhci_enable_slot(void);
int  xhci_address_device(struct usb_device *d, int bsr);
int  xhci_set_ep0_packet(struct usb_device *d, int max_packet);
int  xhci_configure_ep(struct usb_device *d, const struct usb_interface *iface);
void xhci_free_slot(int slot);

/* Transfers. */
int  xhci_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val,
                  uint16_t idx, void *data, uint16_t len);
int  xhci_int_in_arm(struct usb_device *d, uint8_t ep_addr);
/* -> bytes received (>=0) when a report landed, -1 when nothing is ready.
 * *buf is set to the endpoint's DMA buffer. */
int  xhci_int_in_poll(struct usb_device *d, uint8_t ep_addr, uint8_t **buf);

#endif /* LOGIT_XHCI_H */
