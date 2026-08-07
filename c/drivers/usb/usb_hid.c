/* USB HID class driver: keyboards and mice.
 *
 * This file contains no xHCI, no PCI and no enumeration. It is a match table,
 * a probe() and a poll() -- which is the claim c/drivers/usb/usb_core.c exists
 * to make good on.
 *
 * TWO PROTOCOLS, AND WHY BOTH ARE HERE.
 *   Boot protocol (HID 1.11 Appendix B) is a fixed 8-byte keyboard report and a
 *   fixed 3-byte mouse report that a device must emit after SET_PROTOCOL(0),
 *   whatever its report descriptor says. It exists so a BIOS can drive a
 *   keyboard without a HID parser, and it is enough for QEMU's usb-kbd and
 *   usb-mouse. It is NOT enough for a desk: boot protocol is optional
 *   (bInterfaceSubClass 1 is what advertises it), and a wireless combo receiver
 *   multiplexes a keyboard and a mouse down one endpoint by report ID, which
 *   boot protocol cannot express at all.
 *
 *   So the report descriptor is fetched and parsed for every device, and the
 *   PARSED path is the primary decoder -- which means QEMU exercises it on
 *   every boot rather than leaving it as untested code for hardware nobody
 *   here has. Boot protocol is the fallback, taken when the descriptor is
 *   absent, unparseable, or describes something we do not recognise; the
 *   driver says which one it chose on the serial console.
 *
 * WHERE THE EVENTS GO.
 *   wm_key() and wm_mouse_event() in c/kernel/gui/wm.h. Those are the same two
 *   functions c/drivers/char/keyboard.c and mouse.c call, and they only enqueue
 *   onto the window manager's raw input ring -- no shared-state writes, no
 *   locks -- so a second producer is exactly what they were built for. Nothing
 *   in the PS/2 drivers is touched, disabled or displaced, and both paths can
 *   feed the queue at once.
 *
 *   Duplicate delivery does not happen, for two separate reasons that cover the
 *   two cases. Under QEMU an input event is routed to one handler, so a machine
 *   with both a PS/2 and a USB keyboard delivers each keystroke once, through
 *   whichever device QEMU picked. On real hardware the danger is firmware:
 *   before an xHCI driver exists, the BIOS makes a USB keyboard look like a
 *   PS/2 one by trapping into SMM and feeding the 8042, which WOULD deliver
 *   every key twice once this driver starts. xhci.c performs the USB Legacy
 *   Support handoff (xHCI 7.1.1) at bring-up, which is the mechanism that turns
 *   that emulation off. It is not a heuristic and not a race; it is the
 *   handshake the firmware is waiting for.
 */

#include <stdint.h>
#include <stddef.h>
#include "usb.h"
#include "usb_desc.h"
#include "hid_report.h"
#include "xhci.h"
#include "wm.h"
#include "fb.h"
#include "kheap.h"
#include "kprintf.h"
#include "pit.h"
#include "logit_abi.h"

void *memset(void *, int, size_t);

extern unsigned long usb_reports_total, usb_keys_total, usb_motion_total;

#define ROLE_NONE 0
#define ROLE_KBD  1
#define ROLE_MOUSE 2

/* HID modifier byte (boot report byte 0, and the same bit order the parsed
 * decoder produces from usages E0..E7). */
#define HIDM_LCTRL  0x01
#define HIDM_LSHIFT 0x02
#define HIDM_LALT   0x04
#define HIDM_RCTRL  0x10
#define HIDM_RSHIFT 0x20
#define HIDM_RALT   0x40
#define HIDM_SHIFT  (HIDM_LSHIFT | HIDM_RSHIFT)
#define HIDM_CTRL   (HIDM_LCTRL | HIDM_RCTRL)

struct hid_dev {
    struct hid_desc rd;
    uint8_t have_rd;
    uint8_t generic;         /* 1 = decode through the parsed descriptor */
    uint8_t role;
    uint8_t ep_addr;
    uint8_t ifnum;

    /* Keyboard edge detection. A HID keyboard reports the SET of keys that are
     * down, every time anything changes -- not press and release events. The
     * difference between consecutive sets is where keystrokes come from, and
     * failing to diff means holding a key types it forever. */
    uint8_t prev_keys[8];
    int     prev_nkeys;
    uint8_t prev_mods;

    /* Auto-repeat, driven by the DEVICE's idle timer rather than an OS one.
     * A PS/2 keyboard repeats in hardware; a USB one reports only on change, so
     * holding a key would do nothing at all. There is no USB thread to run a
     * repeat timer on (everything here happens in the interrupt handler), so
     * instead SET_IDLE asks the keyboard to re-send its unchanged report every
     * ~96 ms -- which is a periodic interrupt, from the device, for free. An
     * unchanged non-empty report is therefore a repeat tick. */
    int repeat_ticks;

    /* Pointer position. wm_mouse_event() takes an ABSOLUTE screen position and
     * a HID mouse reports relative deltas, so the position is integrated here.
     * See the note in probe() about what that means alongside a PS/2 mouse. */
    int mx, my, have_pos;

    unsigned long reports;
};

/* ------------------------------------------------------------ helpers --- */

static int hid_set_protocol(struct usb_device *d, int ifnum, int proto)
{
    return usb_control(d, USB_RT_TYPE_CLASS | USB_RT_RECIP_IF, HID_REQ_SET_PROTOCOL,
                       (uint16_t)proto, (uint16_t)ifnum, NULL, 0);
}

/* SET_IDLE duration is in 4 ms units; 0 means "report only when something
 * changes" (HID 1.11 7.2.4). A mouse gets 0 -- an idle rate there would deliver
 * a stream of zero-delta reports. A keyboard gets 24 (96 ms), which is what
 * makes auto-repeat possible without an OS timer: see struct hid_dev. */
static int hid_set_idle(struct usb_device *d, int ifnum, int units)
{
    return usb_control(d, USB_RT_TYPE_CLASS | USB_RT_RECIP_IF, HID_REQ_SET_IDLE,
                       (uint16_t)(units << 8), (uint16_t)ifnum, NULL, 0);
}

static int hid_get_report_desc(struct usb_device *d, int ifnum, uint8_t *buf, uint16_t len)
{
    /* Standard GET_DESCRIPTOR, but addressed to the INTERFACE -- the report
     * descriptor belongs to the HID interface, not to the device (HID 1.11
     * 7.1.1), and asking the device for it gets a STALL. */
    return usb_control(d, USB_RT_DIR_IN | USB_RT_TYPE_STD | USB_RT_RECIP_IF,
                       USB_REQ_GET_DESCRIPTOR, (USB_DT_HID_REPORT << 8), (uint16_t)ifnum,
                       buf, len);
}

/* ---------------------------------------------------------- keyboard --- */

static int was_down(const struct hid_dev *h, uint8_t usage)
{
    for (int i = 0; i < h->prev_nkeys; i++)
        if (h->prev_keys[i] == usage) return 1;
    return 0;
}

/* Turn one usage into the code wm_key() takes, and post it. */
static void post_key(uint8_t usage, uint8_t mods)
{
    int shift = (mods & HIDM_SHIFT) != 0;
    int k = hid_usage_to_key(usage, shift);
    if (!k) return;

    /* Ctrl+letter collapses to a control code, exactly as
     * c/drivers/char/keyboard.c does it -- Ctrl+S must be 0x13 whichever
     * keyboard it was typed on, or TextEdit saves from one and not the other. */
    if ((mods & HIDM_CTRL) && k >= 'a' && k <= 'z') k = k - 'a' + 1;
    else if ((mods & HIDM_CTRL) && k >= 'A' && k <= 'Z') k = k - 'A' + 1;

    wm_key(k);
    usb_keys_total++;
}

#define REPEAT_DELAY_TICKS 4      /* ~4 x 96 ms before a held key starts over */

static void handle_keyboard(struct hid_dev *h, const uint8_t *rep, int len)
{
    struct hid_kbd_state ks;

    if (h->generic) {
        if (hid_decode_keyboard(&h->rd, rep, len, &ks) <= 0) return;
    } else {
        if (len < 8) return;
        memset(&ks, 0, sizeof ks);
        ks.mods = rep[0];
        for (int i = 2; i < 8 && ks.nkeys < 8; i++)
            if (rep[i] > 3) ks.keys[ks.nkeys++] = rep[i];
    }

    int fresh = 0;
    for (int i = 0; i < ks.nkeys; i++)
        if (!was_down(h, ks.keys[i])) { post_key(ks.keys[i], ks.mods); fresh = 1; }

    /* Same set of keys still held, nothing new pressed: this is the device's
     * idle re-report, i.e. a repeat tick. The last key in the report is the one
     * most recently pressed, which is the one a person expects to repeat. */
    int same = (ks.nkeys == h->prev_nkeys) && !fresh;
    for (int i = 0; same && i < ks.nkeys; i++)
        if (ks.keys[i] != h->prev_keys[i]) same = 0;

    if (same && ks.nkeys > 0) {
        if (++h->repeat_ticks >= REPEAT_DELAY_TICKS)
            post_key(ks.keys[ks.nkeys - 1], ks.mods);
    } else {
        h->repeat_ticks = 0;
    }

    for (int i = 0; i < 8; i++) h->prev_keys[i] = i < ks.nkeys ? ks.keys[i] : 0;
    h->prev_nkeys = ks.nkeys;
    h->prev_mods = ks.mods;
}

/* ------------------------------------------------------------- mouse --- */

static void handle_mouse(struct hid_dev *h, const uint8_t *rep, int len)
{
    struct hid_mouse_state ms;

    if (h->generic) {
        int ok = hid_decode_mouse(&h->rd, rep, len, &ms);
        if (ok <= 0) return;
    } else {
        if (len < 3) return;
        memset(&ms, 0, sizeof ms);
        ms.buttons = rep[0] & 0x1F;
        ms.dx = (int)(int8_t)rep[1];
        ms.dy = (int)(int8_t)rep[2];
        ms.wheel = len >= 4 ? (int)(int8_t)rep[3] : 0;
    }

    if (!h->have_pos) {
        h->mx = (int)fb_width() / 2;
        h->my = (int)fb_height() / 2;
        h->have_pos = 1;
    }

    /* HID Y grows DOWNWARD (Usage Tables 1.12, Generic Desktop Y), which is the
     * same direction as screen coordinates -- so this adds where the PS/2 path
     * subtracts. PS/2 packets carry Y growing upward; that is a property of the
     * PS/2 protocol, not of mice. */
    h->mx += ms.dx;
    h->my += ms.dy;

    int w = (int)fb_width(), ht = (int)fb_height();
    if (h->mx < 0) h->mx = 0;
    if (h->my < 0) h->my = 0;
    if (h->mx > w - 1) h->mx = w - 1;
    if (h->my > ht - 1) h->my = ht - 1;

    /* Buttons are LEVELS, which is what wm_mouse_event() documents wanting: the
     * window manager derives press and release from the previous level, because
     * only it knows which window owns a press. */
    wm_mouse_event(h->mx, h->my,
                   (ms.buttons & 1) ? 1 : 0,
                   (ms.buttons & 2) ? 1 : 0,
                   (ms.buttons & 4) ? 1 : 0,
                   ms.wheel);
    usb_motion_total++;
}

/* ------------------------------------------------------- probe / poll --- */

static int hid_probe(struct usb_device *d, int ifno)
{
    struct usb_interface *it = &d->cfg.iface[ifno];
    const struct usb_endpoint *ep = usb_find_ep(it, USB_XFER_INT, 1);
    if (!ep) {
        kprintf("[hid] if%d has no interrupt IN endpoint\n", ifno);
        return -1;
    }

    struct hid_dev *h = kmalloc(sizeof *h);
    if (!h) return -1;
    memset(h, 0, sizeof *h);
    h->ep_addr = ep->addr;
    h->ifnum = it->num;

    /* Parse the report descriptor if the interface declared one. */
    if (it->has_hid && it->hid_report_len > 0 && it->hid_report_len <= 512) {
        uint8_t *rd = kmalloc(it->hid_report_len);
        if (rd) {
            int n = hid_get_report_desc(d, it->num, rd, it->hid_report_len);
            if (n > 0 && hid_parse_report_desc(rd, n, &h->rd) == 0) {
                h->have_rd = 1;
                kprintf("USB_HID_RD if=%d bytes=%d fields=%d reports=%d ids=%d\n",
                        it->num, n, h->rd.nfields, h->rd.nreports, h->rd.uses_report_ids);
            } else {
                kprintf("[hid] if%d: report descriptor %s\n", it->num,
                        n > 0 ? "did not parse" : "could not be read");
            }
            kfree(rd);
        }
    }

    /* Role: what the descriptor actually describes beats what the interface
     * protocol byte claims, because a report-protocol-only device commonly
     * declares bInterfaceProtocol 0 and is still a keyboard. */
    if (h->have_rd && hid_looks_like_keyboard(&h->rd))    h->role = ROLE_KBD;
    else if (h->have_rd && hid_looks_like_mouse(&h->rd))  h->role = ROLE_MOUSE;
    else if (it->if_proto == USB_HID_PROTO_KBD)           h->role = ROLE_KBD;
    else if (it->if_proto == USB_HID_PROTO_MOUSE)         h->role = ROLE_MOUSE;

    if (h->role == ROLE_NONE) {
        kprintf("[hid] if%d: neither keyboard nor mouse; not bound\n", it->num);
        kfree(h);
        return -1;
    }

    h->generic = h->have_rd;
    if (!h->generic) {
        /* Fallback. SET_PROTOCOL(boot) is what makes the fixed layout a
         * guarantee rather than a hope, and it only works on a boot-capable
         * interface -- so if this is not one, we have nothing to decode with. */
        if (it->if_subclass != USB_HID_SUB_BOOT) {
            kprintf("[hid] if%d: no usable report descriptor and no boot protocol\n", it->num);
            kfree(h);
            return -1;
        }
        if (hid_set_protocol(d, it->num, HID_PROTO_BOOT) < 0)
            kprintf("[hid] if%d: SET_PROTOCOL(boot) refused; assuming boot layout\n", it->num);
    }

    /* Advisory -- some devices STALL SET_IDLE, harmlessly. A keyboard that
     * refuses it simply will not auto-repeat. */
    hid_set_idle(d, it->num, h->role == ROLE_KBD ? 24 : 0);

    d->drvdata = h;
    if (xhci_int_in_arm(d, h->ep_addr) != 0) {
        kprintf("[hid] if%d: could not arm the interrupt endpoint\n", it->num);
        kfree(h);
        d->drvdata = NULL;
        return -1;
    }

    kprintf("USB_HID_BIND if=%d role=%s decode=%s ep=%02x interval=%d\n",
            it->num, h->role == ROLE_KBD ? "keyboard" : "mouse",
            h->generic ? "report-descriptor" : "boot-protocol",
            h->ep_addr, ep->interval);
    return 0;
}

static void hid_poll(struct usb_device *d)
{
    struct hid_dev *h = d->drvdata;
    if (!h) return;

    uint8_t *buf = NULL;
    int n = xhci_int_in_poll(d, h->ep_addr, &buf);
    if (n >= 0 && buf) {
        h->reports++;
        usb_reports_total++;
        if (h->role == ROLE_KBD) handle_keyboard(h, buf, n);
        else                     handle_mouse(h, buf, n);
    }

    /* Re-arm unconditionally: an interrupt endpoint with no TRB queued is an
     * endpoint the controller stops asking about, and the report that would
     * have told us so is the one we are no longer collecting. */
    xhci_int_in_arm(d, h->ep_addr);
}

static void hid_remove(struct usb_device *d)
{
    if (d->drvdata) { kfree(d->drvdata); d->drvdata = NULL; }
}

/* Any HID interface, whatever its subclass or protocol. The role is decided in
 * probe() from the report descriptor, so matching narrowly on the boot triples
 * would exclude exactly the devices the parser exists for. */
static const struct usb_match hid_ids[] = {
    { USB_CLASS_HID, USB_ANY, USB_ANY },
    { 0, 0, 0 }
};

static const struct usb_driver hid_driver = {
    .name = "hid",
    .match = hid_ids,
    .probe = hid_probe,
    .poll = hid_poll,
    .remove = hid_remove,
};

void usb_hid_register(void) { usb_register_driver(&hid_driver); }
