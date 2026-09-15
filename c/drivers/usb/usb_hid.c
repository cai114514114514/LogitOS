/* USB HID class driver: keyboards, mice, generic gamepads and touchpads.
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
#include "hid/generic.h"
#include "hid/feature.h"
#include "usb_hc.h"
#include "wm.h"
#include "fb.h"
#include "kheap.h"
#include "kprintf.h"
#include "pit.h"
#include "logit_abi.h"

#ifndef memset
void *memset(void *, int, size_t);
#endif

extern unsigned long usb_reports_total, usb_keys_total, usb_motion_total;

#define ROLE_NONE 0
#define ROLE_KBD 1
#define ROLE_MOUSE 2

/* The common keyboard reader may run on another CPU. Keep snapshots outside
 * allocated HID state so a read cannot race interface removal's kfree. Each
 * interface owns one slot; removal clears it after core drains IRQ users. */
static unsigned hid_modifiers[USB_MAX_DEVICES * USB_MAX_IF];
static uint64_t hid_held[USB_MAX_DEVICES * USB_MAX_IF][4];
static uint64_t hid_game_held[USB_MAX_DEVICES * USB_MAX_IF][4];
static unsigned hid_pointer_buttons[USB_MAX_DEVICES * USB_MAX_IF];
static unsigned char hid_modifier_used[USB_MAX_DEVICES * USB_MAX_IF];
int usb_hid_mods(void)
{
    unsigned m = 0;
    for (unsigned i = 0; i < USB_MAX_DEVICES * USB_MAX_IF; i++)
        m |= __atomic_load_n(&hid_modifiers[i], __ATOMIC_ACQUIRE);
    return ((m & 0x22) ? EV_MOD_SHIFT : 0) | ((m & 0x11) ? EV_MOD_CTRL : 0) |
           ((m & 0x44) ? EV_MOD_ALT : 0) | ((m & 0x88) ? EV_MOD_SUPER : 0);
}

/* Query physical levels, independent of text translation and typematic.
 * Static snapshots survive interface teardown; never let a game query an
 * allocated hid_dev that the USB removal path can free concurrently. */
int usb_hid_key_held(int key)
{
    if (key >= 'A' && key <= 'Z')
        key += 'a' - 'A';
    uint64_t held[4] = {0};
    for (unsigned slot = 0; slot < USB_MAX_DEVICES * USB_MAX_IF; slot++) {
        for (int word = 0; word < 4; word++) {
            held[word] |= __atomic_load_n(&hid_held[slot][word], __ATOMIC_ACQUIRE);
            held[word] |= __atomic_load_n(&hid_game_held[slot][word], __ATOMIC_ACQUIRE);
        }
    }
    for (unsigned usage = 4; usage < 256; usage++) {
        int usage_held = !!(held[usage / 64] & (1ull << (usage % 64)));
        if (usage_held && hid_usage_to_key(usage, 0) == key)
            return 1;
    }
    return 0;
}

/* HID modifier byte (boot report byte 0, and the same bit order the parsed
 * decoder produces from usages E0..E7). */
#define HIDM_LCTRL 0x01
#define HIDM_LSHIFT 0x02
#define HIDM_LALT 0x04
#define HIDM_RCTRL 0x10
#define HIDM_RSHIFT 0x20
#define HIDM_RALT 0x40
#define HIDM_SHIFT (HIDM_LSHIFT | HIDM_RSHIFT)
#define HIDM_CTRL (HIDM_LCTRL | HIDM_RCTRL)

struct hid_dev {
    struct hid_desc rd;
    uint8_t have_rd;
    uint8_t generic; /* 1 = decode through the parsed descriptor */
    uint8_t role;
    uint8_t ep_addr;
    uint8_t ifnum;

    /* Keyboard edge detection. A HID keyboard reports the SET of keys that are
     * down, every time anything changes -- not press and release events. The
     * difference between consecutive sets is where keystrokes come from, and
     * failing to diff means holding a key types it forever. */
    struct hid_history {
        uint8_t prev_keys[HID_MAX_KEYS];
        int prev_nkeys;
        uint8_t prev_mods;
        int repeat_ticks;
    } history[HID_MAX_REPORTS];
    int modifier_slot;

    /* Auto-repeat, driven by the DEVICE's idle timer rather than an OS one.
     * A PS/2 keyboard repeats in hardware; a USB one reports only on change, so
     * holding a key would do nothing at all. There is no USB thread to run a
     * repeat timer on (everything here happens in the interrupt handler), so
     * instead SET_IDLE asks the keyboard to re-send its unchanged report every
     * ~96 ms -- which is a periodic interrupt, from the device, for free. An
     * unchanged non-empty report is therefore a repeat tick. */

    /* Pointer position. wm_mouse_event() takes an ABSOLUTE screen position and
     * a HID mouse reports relative deltas, so the position is integrated here.
     * See the note in probe() about what that means alongside a PS/2 mouse. */
    int mx, my, have_pos;
    uint32_t buttons;

    struct hid_gamepad_state gamepad;
    struct hid_kbd_state game_keys;
    struct hid_touch_state touch;
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
    return usb_control(d, USB_RT_DIR_IN | USB_RT_TYPE_STD | USB_RT_RECIP_IF, USB_REQ_GET_DESCRIPTOR,
                       (USB_DT_HID_REPORT << 8), (uint16_t)ifnum, buf, len);
}

/* ---------------------------------------------------------- keyboard --- */

static int was_down(const struct hid_history *h, uint8_t usage)
{
    for (int i = 0; i < h->prev_nkeys; i++)
        if (h->prev_keys[i] == usage)
            return 1;
    return 0;
}

/* Turn one usage into the code wm_key() takes, and post it. */
static void post_key(uint8_t usage, uint8_t mods)
{
    int shift = (mods & HIDM_SHIFT) != 0;
    int k = hid_usage_to_key(usage, shift);
    if (!k)
        return;

    /* Ctrl+letter collapses to a control code, exactly as
     * c/drivers/char/keyboard.c does it -- Ctrl+S must be 0x13 whichever
     * keyboard it was typed on, or TextEdit saves from one and not the other. */
    if ((mods & HIDM_CTRL) && k >= 'a' && k <= 'z')
        k = k - 'a' + 1;
    else if ((mods & HIDM_CTRL) && k >= 'A' && k <= 'Z')
        k = k - 'A' + 1;

    wm_key(k);
    usb_keys_total++;
}

#define REPEAT_DELAY_TICKS 4 /* ~4 x 96 ms before a held key starts over */

static void handle_keyboard(struct hid_dev *h, const uint8_t *rep, int len)
{
    struct hid_kbd_state ks;
    int report = 0;

    if (h->generic) {
        if (hid_decode_keyboard(&h->rd, rep, len, &ks) <= 0)
            return;
        if (h->rd.uses_report_ids)
            for (int i = 0; i < h->rd.nreports; i++)
                if (h->rd.rep[i].id == rep[0]) {
                    report = i;
                    break;
                }
    } else {
        if (len < 8)
            return;
        memset(&ks, 0, sizeof ks);
        ks.mods = rep[0];
        for (int i = 2; i < 8 && ks.nkeys < 8; i++)
            if (rep[i] > 3)
                ks.keys[ks.nkeys++] = rep[i];
    }

    /* A receiver can put modifiers and normal keys in separate report IDs.
     * Keep their histories independent and publish the union BEFORE wm_key
     * samples kbd_mods, so the first shifted key/click has the right flags. */
    struct hid_history *prev = &h->history[report];
    prev->prev_mods = ks.mods;
    unsigned combined = 0;
    for (int i = 0; i < HID_MAX_REPORTS; i++)
        combined |= h->history[i].prev_mods;
    __atomic_store_n(&hid_modifiers[h->modifier_slot], combined, __ATOMIC_RELEASE);
    uint64_t held[4] = {0};
    for (int r = 0; r < HID_MAX_REPORTS; r++) {
        const uint8_t *keys = r == report ? ks.keys : h->history[r].prev_keys;
        int n = r == report ? ks.nkeys : h->history[r].prev_nkeys;
        for (int i = 0; i < n; i++)
            held[keys[i] / 64] |= 1ull << (keys[i] % 64);
    }
    for (int i = 0; i < 4; i++)
        __atomic_store_n(&hid_held[h->modifier_slot][i], held[i], __ATOMIC_RELEASE);
    int fresh = 0;
    for (int i = 0; i < ks.nkeys; i++)
        if (!was_down(prev, ks.keys[i])) {
            post_key(ks.keys[i], combined);
            fresh = 1;
        }

    /* Same set of keys still held, nothing new pressed: this is the device's
     * idle re-report, i.e. a repeat tick. The last key in the report is the one
     * most recently pressed, which is the one a person expects to repeat. */
    int same = (ks.nkeys == prev->prev_nkeys) && !fresh;
    for (int i = 0; same && i < ks.nkeys; i++)
        if (ks.keys[i] != prev->prev_keys[i])
            same = 0;

    if (same && ks.nkeys > 0) {
        if (++prev->repeat_ticks >= REPEAT_DELAY_TICKS)
            post_key(ks.keys[ks.nkeys - 1], combined);
    } else {
        prev->repeat_ticks = 0;
    }

    for (int i = 0; i < HID_MAX_KEYS; i++)
        prev->prev_keys[i] = i < ks.nkeys ? ks.keys[i] : 0;
    prev->prev_nkeys = ks.nkeys;
}

/* ------------------------------------------------------------- mouse --- */

/* Per-interface button levels keep unplugging one USB pointer from releasing
 * a different USB pointer's held click. USB core drains callbacks before remove. */
static void publish_pointer_event(struct hid_dev *device, int wheel)
{
    __atomic_store_n(&hid_pointer_buttons[device->modifier_slot], device->buttons,
                     __ATOMIC_RELEASE);
    unsigned combined_buttons = 0;
    for (unsigned slot = 0; slot < USB_MAX_DEVICES * USB_MAX_IF; slot++)
        combined_buttons |= __atomic_load_n(&hid_pointer_buttons[slot], __ATOMIC_ACQUIRE);

    wm_mouse_event(device->mx, device->my, !!(combined_buttons & 1), !!(combined_buttons & 2),
                   !!(combined_buttons & 4), wheel);
    usb_motion_total++;
}

static void initialize_pointer_position(struct hid_dev *device)
{
    if (device->have_pos)
        return;
    device->mx = (int)fb_width() / 2;
    device->my = (int)fb_height() / 2;
    device->have_pos = 1;
}

static void publish_gamepad_keys(struct hid_dev *device, const struct hid_kbd_state *keys)
{
    /* Held levels are published before text edges, matching the keyboard
     * path. Removal can clear these static snapshots without exposing freed
     * interface state to a game's concurrent held-key query. */
    uint64_t held[4] = {0};
    for (int i = 0; i < keys->nkeys; i++) {
        uint8_t usage = keys->keys[i];
        held[usage / 64] |= 1ull << (usage % 64);
    }
    for (int word = 0; word < 4; word++)
        __atomic_store_n(&hid_game_held[device->modifier_slot][word], held[word], __ATOMIC_RELEASE);

    for (int i = 0; i < keys->nkeys; i++) {
        int was_held = 0;
        for (int previous = 0; previous < device->game_keys.nkeys; previous++) {
            if (keys->keys[i] == device->game_keys.keys[previous])
                was_held = 1;
        }
        if (!was_held)
            post_key(keys->keys[i], 0);
    }
    device->game_keys = *keys;
}

static void handle_gamepad(struct hid_dev *device, const uint8_t *packet, int length)
{
    struct hid_gamepad gamepad;
    struct hid_kbd_state keys = {0};
    int result = hid_gamepad_decode(&device->rd, packet, length, &device->gamepad, &gamepad);
    if (result == 0)
        return;
    if (result > 0)
        hid_gamepad_keys(&gamepad, &keys);
    else
        memset(&device->gamepad, 0, sizeof device->gamepad);
    publish_gamepad_keys(device, &keys);
}

static void release_touch_state(struct hid_dev *device)
{
    /* A malformed frame cannot keep an old drag/contact alive. Resetting
     * anchors also means the next valid contact starts without a jump. */
    memset(&device->touch, 0, sizeof device->touch);
    device->buttons = 0;
    publish_pointer_event(device, 0);
}

static void handle_touch(struct hid_dev *device, const uint8_t *packet, int length)
{
    struct hid_touch_frame frame;
    int result = hid_touch_decode(&device->rd, packet, length, &frame);
    if (result == 0)
        return;

    initialize_pointer_position(device);
    if (result < 0) {
        release_touch_state(device);
        return;
    }
    int report_slot = 0;
    for (int slot = 0; slot < device->rd.nreports; slot++) {
        if (device->rd.rep[slot].id == frame.report_id) {
            report_slot = slot;
            break;
        }
    }

    struct hid_pointer_delta delta;
    hid_touch_apply(&device->touch, &frame, report_slot, &delta);
    struct hid_mouse_state motion = {.dx = delta.dx,
                                     .dy = delta.dy,
                                     .buttons = delta.buttons,
                                     .present =
                                         HID_MOUSE_HAS_X | HID_MOUSE_HAS_Y | HID_MOUSE_HAS_BUTTONS};
    hid_mouse_apply(&motion, (int)fb_width(), (int)fb_height(), &device->mx, &device->my,
                    &device->buttons);
    publish_pointer_event(device, delta.wheel);
}

static void handle_mouse(struct hid_dev *h, const uint8_t *rep, int len)
{
    struct hid_mouse_state ms;

    if (h->generic) {
        int ok = hid_decode_mouse(&h->rd, rep, len, &ms);
        if (ok <= 0)
            return;
    } else {
        if (len < 3)
            return;
        memset(&ms, 0, sizeof ms);
        ms.buttons = rep[0] & 0x1F;
        ms.present = 15;
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
    /* Correction: the old addition above treated absolute tablet coordinates
     * as deltas. Repeated identical reports ran the pointer to a screen edge.
     * HID 1.11 6.2.2.5's Relative bit and each axis's logical range decide the
     * conversion; no product IDs or QEMU-specific coordinate limits appear. */
    hid_mouse_apply(&ms, (int)fb_width(), (int)fb_height(), &h->mx, &h->my, &h->buttons);

    /* THE WHEEL IS NEGATED, and this is not a guess. HID Usage(Wheel) is
     * positive for rotation AWAY from the user, i.e. scrolling up (Usage Tables
     * 1.12, 4.2). EV_WHEEL is positive for scrolling DOWN -- that is what the
     * PS/2 path produces, what tests/qmp/qmp_input.py pins down, and what the
     * DOM's deltaY means, and browser.aex reads it as deltaY. The two
     * conventions genuinely disagree, so somebody has to flip, and it is the
     * driver whose protocol disagrees with the ABI.
     *
     * Caught by measurement, not by reading: the first version passed this
     * value through, and the harness's original assertion ("some notch is
     * positive and some is negative") was satisfied by the inverted result.
     * The assertion now checks WHICH direction produced which sign.
     *
     * Buttons are LEVELS, which is what wm_mouse_event() documents wanting: the
     * window manager derives press and release from the previous level, because
     * only it knows which window owns a press. */
    publish_pointer_event(h, ms.wheel == INT32_MIN ? INT32_MAX : -ms.wheel);
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
    if (!h)
        return -1;
    memset(h, 0, sizeof *h);
    h->ep_addr = ep->addr;
    h->ifnum = it->num;

    /* Parse the report descriptor if the interface declared one. */
    if (it->has_hid && it->hid_report_len > 0 && it->hid_report_len <= 4096) {
        uint8_t *rd = kmalloc(it->hid_report_len);
        if (rd) {
            int n = hid_get_report_desc(d, it->num, rd, it->hid_report_len);
            if (n > 0 && hid_parse_report_desc(rd, n, &h->rd) == 0) {
                h->have_rd = 1;
                kprintf("USB_HID_RD if=%d bytes=%d fields=%d reports=%d ids=%d\n", it->num, n,
                        h->rd.nfields, h->rd.nreports, h->rd.uses_report_ids);
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
    if (h->have_rd) {
        if (hid_looks_like_keyboard(&h->rd))
            h->role |= ROLE_KBD;
        if (hid_looks_like_mouse(&h->rd))
            h->role |= ROLE_MOUSE;
        h->role |= hid_generic_roles(&h->rd);
    } else if (it->if_proto == USB_HID_PROTO_KBD)
        h->role = ROLE_KBD;
    else if (it->if_proto == USB_HID_PROTO_MOUSE)
        h->role = ROLE_MOUSE;

    if (h->role == ROLE_NONE) {
        kprintf("[hid] if%d: no supported HID input collection; not bound\n", it->num);
        kfree(h);
        return -1;
    }

    /* This transport queues one interrupt packet per report. A descriptor
     * needing more than that cannot be decoded atomically; do not bind it and
     * turn every transfer into a plausible but truncated contact frame. */
    if (h->have_rd) {
        unsigned packet_bytes = ep->max_packet & 0x7ffu;
        for (int slot = 0; slot < h->rd.nreports; slot++) {
            unsigned body_bytes = (h->rd.rep[slot].in_bits + 7u) / 8u;
            unsigned prefix_bytes = h->rd.uses_report_ids ? 1u : 0u;
            if (body_bytes + prefix_bytes > packet_bytes) {
                kfree(h);
                return -1;
            }
        }
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
        if (hid_set_protocol(d, it->num, HID_PROTO_BOOT) < 0) {
            /* The former "assuming boot layout" on a refused request silently
             * interpreted unverified bytes as keys. A STALL gives no layout
             * guarantee; leave this interface unbound. */
            kprintf("[hid] if%d: SET_PROTOCOL(boot) refused\n", it->num);
            kfree(h);
            return -1;
        }
    } else if (it->if_subclass == USB_HID_SUB_BOOT &&
               hid_set_protocol(d, it->num, HID_PROTO_REPORT) < 0) {
        /* Firmware may have left a boot keyboard in boot protocol. Parsing its
         * report descriptor is not sufficient to select that wire format. */
        kprintf("[hid] if%d: SET_PROTOCOL(report) refused\n", it->num);
        kfree(h);
        return -1;
    }

    if ((h->role & HID_ROLE_TOUCH) && hid_touchpad_configure(d, it->num, &h->rd) < 0) {
        kprintf("[hid] if%d: touchpad mode negotiation refused\n", it->num);
        kfree(h);
        return -1;
    }

    /* Advisory -- some devices STALL SET_IDLE, harmlessly. A keyboard that
     * refuses it simply will not auto-repeat. */
    hid_set_idle(d, it->num, (h->role & ROLE_KBD) ? 24 : 0);

    h->modifier_slot = -1;
    for (unsigned i = 0; i < USB_MAX_DEVICES * USB_MAX_IF; i++)
        if (!hid_modifier_used[i]) {
            hid_modifier_used[i] = 1;
            h->modifier_slot = (int)i;
            break;
        }
    if (h->modifier_slot < 0) {
        kfree(h);
        return -1;
    }
    d->binding[ifno].drvdata = h;
    if (usb_int_in_arm(d, h->ep_addr) != 0) {
        kprintf("[hid] if%d: could not arm the interrupt endpoint\n", it->num);
        hid_modifier_used[h->modifier_slot] = 0;
        kfree(h);
        d->binding[ifno].drvdata = NULL;
        return -1;
    }

    kprintf("USB_HID_BIND if=%d role=%s decode=%s ep=%02x interval=%d\n", it->num,
            h->role == (ROLE_KBD | ROLE_MOUSE) ? "keyboard+mouse"
            : h->role == ROLE_KBD              ? "keyboard"
            : h->role == ROLE_MOUSE            ? "mouse"
            : h->role == HID_ROLE_GAMEPAD      ? "gamepad"
            : h->role == HID_ROLE_TOUCH        ? "touchpad"
                                               : "composite",
            h->generic ? "report-descriptor" : "boot-protocol", h->ep_addr, ep->interval);
    return 0;
}

static void hid_poll(struct usb_device *d, int ifno)
{
    struct hid_dev *h = d->binding[ifno].drvdata;
    if (!h)
        return;

    uint8_t *buf = NULL;
    int n = usb_int_in_poll(d, h->ep_addr, &buf);
    if (n >= 0 && buf) {
        h->reports++;
        usb_reports_total++;
        /* A single HID interface can multiplex both roles using report IDs.
         * Each decoder rejects reports outside its fields. The old else made
         * every mouse report vanish when the descriptor also had a keyboard. */
        if (h->role & ROLE_KBD)
            handle_keyboard(h, buf, n);
        if (h->role & ROLE_MOUSE)
            handle_mouse(h, buf, n);
        if (h->role & HID_ROLE_GAMEPAD)
            handle_gamepad(h, buf, n);
        if (h->role & HID_ROLE_TOUCH)
            handle_touch(h, buf, n);
    }

    /* Re-arm unconditionally: an interrupt endpoint with no TRB queued is an
     * endpoint the controller stops asking about, and the report that would
     * have told us so is the one we are no longer collecting. */
    usb_int_in_arm(d, h->ep_addr);
}

static void hid_remove(struct usb_device *d, int ifno)
{
    struct hid_dev *h = d->binding[ifno].drvdata;
    if (h) {
        __atomic_store_n(&hid_modifiers[h->modifier_slot], 0, __ATOMIC_RELEASE);
        for (int i = 0; i < 4; i++)
            __atomic_store_n(&hid_held[h->modifier_slot][i], 0, __ATOMIC_RELEASE);
        for (int word = 0; word < 4; word++)
            __atomic_store_n(&hid_game_held[h->modifier_slot][word], 0, __ATOMIC_RELEASE);
        if (h->buttons) {
            h->buttons = 0;
            publish_pointer_event(h, 0);
        }
        __atomic_store_n(&hid_pointer_buttons[h->modifier_slot], 0, __ATOMIC_RELEASE);
        hid_modifier_used[h->modifier_slot] = 0;
        kfree(h);
        d->binding[ifno].drvdata = NULL;
    }
}

/* Any HID interface, whatever its subclass or protocol. The role is decided in
 * probe() from the report descriptor, so matching narrowly on the boot triples
 * would exclude exactly the devices the parser exists for. */
static const struct usb_match hid_ids[] = {{USB_CLASS_HID, USB_ANY, USB_ANY}, {0, 0, 0}};

static const struct usb_driver hid_driver = {
    .name = "hid",
    .match = hid_ids,
    .probe = hid_probe,
    .poll = hid_poll,
    .remove = hid_remove,
};

void usb_hid_register(void)
{
    usb_register_driver(&hid_driver);
}
