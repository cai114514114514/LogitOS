/* Per-interface HID traffic counters, for the hardware panel (hwhud.c).
 * Read without locks: diagnostic values, never decisions. */
#ifndef HID_DIAG_H
#define HID_DIAG_H
#include <stdint.h>

struct usb_hid_diag {
    uint16_t vid, pid;
    uint8_t ifnum, role;            /* role bit 0 keyboard, bit 1 mouse */
    uint8_t last_len, last[8];      /* the most recent report, first 8 bytes */
    unsigned long reports;          /* interrupt reports received */
    unsigned long kbd_ok, kbd_rej;  /* keyboard decodes accepted / refused */
    unsigned long kbd_down;         /* new key presses posted */
};
/* 0 and *out filled for bound interface i, -1 past the last one. */
int usb_hid_diag_get(int i, struct usb_hid_diag *out);

#endif
