/* Per-interface HID bring-up record and traffic counters, for the hardware
 * panel (hwhud.c). Read without locks: diagnostic values, never decisions.
 *
 * WHY THE BRING-UP FACTS LIVE HERE. A USB keyboard that never delivers a report
 * looks identical on the panel whether the device refused a class request,
 * sits in the wrong configuration, uses the other protocol, or simply waits
 * for the host's LED report (see usb_hid.c). Recording every request's outcome
 * next to the counters lets one photograph of the screen discriminate those
 * cases instead of costing another hardware round trip.
 * Not verified on X79-VG4 hardware. */
#ifndef HID_DIAG_H
#define HID_DIAG_H
#include <stdint.h>

/* Outcome of one request. Zero is "never sent" so a freshly cleared slot
 * reads correctly without any initialisation. */
#define HID_DIAG_REQ_UNSENT 0
#define HID_DIAG_REQ_OK     1
#define HID_DIAG_REQ_FAILED 2

/* Which decoder hid_probe() chose (HID 1.11 Appendix B boot layout versus the
 * parsed report descriptor). */
#define HID_DIAG_DECODE_NONE       0
#define HID_DIAG_DECODE_DESCRIPTOR 1
#define HID_DIAG_DECODE_BOOT       2

/* Control-pipe fallback life cycle (usb_hid.c, hid_service). */
#define HID_DIAG_CTL_NONE    0   /* not a keyboard, or no input report to poll */
#define HID_DIAG_CTL_WAIT    1   /* interrupt endpoint armed and silent, 3 s not over */
#define HID_DIAG_CTL_POLL    2   /* the control pipe is the input path ("poll ctl") */
#define HID_DIAG_CTL_RETIRED 3   /* an interrupt report arrived: fallback is over */
#define HID_DIAG_CTL_FAILED  4   /* GET_REPORT is unusable on this interface */

/* SET_PROTOCOL value that was sent: none, boot (0) or report (1). */
#define HID_DIAG_PROTO_NONE   0
#define HID_DIAG_PROTO_BOOT   1
#define HID_DIAG_PROTO_REPORT 2

struct usb_hid_diag {
    uint16_t vid, pid;
    uint8_t ifnum, role;            /* role bit 0 keyboard, bit 1 mouse */
    uint8_t last_len, last[8];      /* the most recent report, first 8 bytes */
    unsigned long reports;          /* interrupt reports received */
    unsigned long kbd_ok, kbd_rej;  /* keyboard decodes accepted / refused */
    unsigned long kbd_down;         /* new key presses posted */

    /* What the interface descriptor and the endpoint say (USB 2.0 9.6.5/9.6.6). */
    uint8_t subclass, protocol;     /* bInterfaceSubClass / bInterfaceProtocol */
    uint8_t decoder;                /* HID_DIAG_DECODE_* */
    uint16_t max_packet;            /* interrupt IN wMaxPacketSize */
    uint8_t interval;               /* interrupt IN bInterval */

    /* Every request the probe issued. *_rc is HID_DIAG_REQ_*. */
    uint8_t cfg_rc, cfg_value;      /* GET_CONFIGURATION, read once after probe */
    uint8_t idle_rc, idle_units;    /* SET_IDLE and its duration in 4 ms units */
    uint8_t proto_rc, proto_sent;   /* SET_PROTOCOL and HID_DIAG_PROTO_* */
    uint8_t rdesc_rc;               /* GET_DESCRIPTOR(report) */
    uint16_t rdesc_len;             /* bytes it returned */
    uint8_t led_rc, led_id, led_len;/* LED SET_REPORT(Output): report id, bytes */

    /* Control-pipe fallback. Counters are uint32_t so the panel row has a
     * bounded width. */
    uint8_t ctl_state;              /* HID_DIAG_CTL_* */
    uint8_t ctl_rc;                 /* last GET_REPORT(Input) */
    uint8_t ctl_len, ctl_first[8];  /* its length and first bytes */
    uint32_t ctl_polls, ctl_fed, ctl_fail;
};
/* 0 and *out filled for bound interface i, -1 past the last one. */
int usb_hid_diag_get(int i, struct usb_hid_diag *out);

#endif
