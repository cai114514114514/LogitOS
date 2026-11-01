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
 * BRING-UP ORDER, AND WHAT LINUX DOES INSTEAD.
 *   hid_probe() follows Linux's usbhid (v6.12 drivers/hid/usbhid/hid-core.c)
 *   in the three places a keyboard can care about:
 *     - usbhid never sends SET_PROTOCOL. A device is in report protocol after
 *       reset (HID 1.11 7.2.6) and usbhid relies on that, so a parsed
 *       descriptor is decoded as-is. Boot protocol is requested here only on
 *       the fallback path, where there is no descriptor to decode with.
 *     - usbhid_start() sends the keyboard LED report to a boot keyboard before
 *       it ever collects input: "Some keyboards don't work until their LEDs
 *       have been set. Since BIOSes do set the LEDs, it must be safe for any
 *       device that supports the keyboard boot protocol." So does this probe,
 *       before it arms the interrupt endpoint.
 *     - usbhid sets NOGET on boot keyboards ("Many keyboards and mice don't
 *       like to be polled for reports"), so it never issues GET_REPORT to one.
 *       hid_service() below does, but only after the interrupt pipe has been
 *       silent for 3 s, and stops for good at the first interrupt report.
 *   Every request's outcome is kept in struct usb_hid_diag for the panel.
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
#include "ktime.h"
#include "logit_abi.h"
#include "hid_diag.h"

#ifndef memset
void *memset(void *, int, size_t);
#endif

extern unsigned long usb_reports_total, usb_keys_total, usb_motion_total;

#define ROLE_NONE 0
#define ROLE_KBD 1
#define ROLE_MOUSE 2

/* Control-pipe fallback tuning (see hid_service). Silence is judged against the
 * interrupt pipe only: 3 s is long enough that SET_IDLE(24)'s 96 ms re-reports
 * would certainly have arrived from a keyboard that honours them. 16 ms is the
 * poll period asked of the core (its timer rounds that up to its own tick), and
 * 96 ms is the cadence at which SET_IDLE(24) would have repeated a held key,
 * kept so auto-repeat feels identical on either path. */
#define HID_CTL_SILENCE_NS (3 * NS_PER_SEC)
#define HID_CTL_POLL_NS (16 * NS_PER_MS)
#define HID_CTL_REPEAT_NS (96 * NS_PER_MS)
#define HID_CTL_GIVE_UP 8       /* consecutive all-failed passes before giving up */
#define HID_CTL_MAX_REPORT 64   /* bytes of one GET_REPORT / SET_REPORT body */

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

/* One input report the control-pipe fallback asks for. */
struct hid_ctl_report {
    uint8_t id;                 /* report ID, 0 when the descriptor has none */
    uint8_t len;                /* GET_REPORT wLength, ID byte included */
    uint8_t prev_len;           /* what was last fed to handle_keyboard() */
    uint8_t prev[HID_CTL_MAX_REPORT];
    uint64_t repeat_ns;         /* when that report was last fed */
};

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
    struct usb_hid_diag *diag;      /* a static slot: outlives this struct */

    /* Control-pipe fallback (hid_service). ctl_state moves between WAIT, POLL
     * and RETIRED with compare-and-swap: the IRQ callback retires it, the
     * kworker promotes it, and neither may overwrite the other's decision. */
    uint8_t ctl_state;              /* HID_DIAG_CTL_* */
    uint8_t ctl_fail_run;
    uint8_t ctl_nreports;
    uint64_t armed_ns;              /* when the interrupt endpoint was armed */
    struct hid_ctl_report ctl[HID_MAX_REPORTS];
};

/* The counters live in static slots, never inside a hid_dev, so the panel
 * can copy them while the hotplug worker frees the device (Astra round 6). */
#define HID_DIAG_SLOTS 16
static struct usb_hid_diag hid_diag_slots[HID_DIAG_SLOTS];
static int hid_diag_used[HID_DIAG_SLOTS];
static struct usb_hid_diag hid_diag_spare;   /* when every slot is taken */

int usb_hid_diag_get(int i, struct usb_hid_diag *out)
{
    for (int k = 0, seen = 0; k < HID_DIAG_SLOTS; k++) {
        if (!__atomic_load_n(&hid_diag_used[k], __ATOMIC_ACQUIRE)) continue;
        if (seen++ == i) { *out = hid_diag_slots[k]; return 0; }
    }
    return -1;
}

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

/* GET_REPORT (HID 1.11 7.2.1): ask the device for its CURRENT input report
 * over the control pipe, the transport that does not depend on the interrupt
 * endpoint being scheduled. wValue is report type in the high byte and report
 * ID in the low byte; the data stage carries the report with its ID byte first
 * when the descriptor uses IDs, exactly as an interrupt transfer would. */
static int hid_get_report(struct usb_device *d, int ifnum, uint8_t report_id, uint8_t *buf,
                          uint16_t len)
{
    return usb_control(d, USB_RT_DIR_IN | USB_RT_TYPE_CLASS | USB_RT_RECIP_IF, HID_REQ_GET_REPORT,
                       (uint16_t)((HID_REPORT_INPUT << 8) | report_id), (uint16_t)ifnum, buf,
                       len);
}

/* SET_REPORT (HID 1.11 7.2.2): send an Output or Feature report. The data
 * stage starts with the report ID byte when the descriptor uses IDs. */
static int hid_set_report(struct usb_device *d, int ifnum, int report_type, uint8_t report_id,
                          void *buf, uint16_t len)
{
    return usb_control(d, USB_RT_TYPE_CLASS | USB_RT_RECIP_IF, HID_REQ_SET_REPORT,
                       (uint16_t)((report_type << 8) | report_id), (uint16_t)ifnum, buf, len);
}

/* GET_CONFIGURATION (USB 2.0 9.4.2): which configuration the device believes
 * it is in. 0 would mean it is not configured and every endpoint is dead. */
static int hid_get_configuration(struct usb_device *d, uint8_t *value)
{
    return usb_control(d, USB_RT_DIR_IN | USB_RT_TYPE_STD | USB_RT_RECIP_DEV,
                       USB_REQ_GET_CONFIGURATION, 0, 0, value, 1);
}

/* WHY: Linux's usbhid_start() calls usbhid_set_leds() for every boot keyboard
 * because "some keyboards don't work until their LEDs have been set" (v6.12
 * drivers/hid/usbhid/hid-core.c), and the BIOS legacy-keyboard path does the
 * same, so a keyboard shipped against either may wait for it. The report is the
 * one the descriptor itself declares (HID 1.11 6.2.2.4 Output item on the LED
 * page) with every LED off; with no descriptor it is the one-byte boot keyboard
 * output report of Appendix B.1, ID 0. Sent over the control pipe because only
 * the interrupt IN endpoint is configured. A refusal is recorded, not fatal: the
 * report is a courtesy to the device, and a keyboard without LEDs may STALL it.
 * Returns whether a request was sent. */
static int hid_send_led_report(struct usb_device *d, const struct hid_dev *h,
                               const struct usb_interface *it, struct usb_hid_diag *log)
{
    uint8_t report_id = 0;
    unsigned body_bytes = 1;
    if (!(h->role & ROLE_KBD))
        return 0;
    if (h->generic && !hid_find_led_output(&h->rd, &report_id, &body_bytes))
        return 0;
    unsigned length = body_bytes + (report_id ? 1u : 0u);
    if (length > HID_CTL_MAX_REPORT)
        return 0;
#ifdef USB_HID_NEGCTL_NO_LED
    return 0;
#endif
    uint8_t report[HID_CTL_MAX_REPORT];
    memset(report, 0, sizeof report);
    if (report_id)
        report[0] = report_id;
    int result = hid_set_report(d, it->num, HID_REPORT_OUTPUT, report_id, report, (uint16_t)length);
    log->led_rc = result < 0 ? HID_DIAG_REQ_FAILED : HID_DIAG_REQ_OK;
    log->led_id = report_id;
    log->led_len = (uint8_t)length;
    kprintf("[hid] if%d LED SET_REPORT id=%u len=%u %s\n", it->num, report_id, length,
            result < 0 ? "refused" : "ok");
    return 1;
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
        if (hid_decode_keyboard(&h->rd, rep, len, &ks) <= 0) {
            h->diag->kbd_rej++;
            return;
        }
        h->diag->kbd_ok++;
        if (h->rd.uses_report_ids)
            for (int i = 0; i < h->rd.nreports; i++)
                if (h->rd.rep[i].id == rep[0]) {
                    report = i;
                    break;
                }
    } else {
        if (len < 8) {
            h->diag->kbd_rej++;
            return;
        }
        h->diag->kbd_ok++;
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
            h->diag->kbd_down++;
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

/* ----------------------------------------- control-pipe fallback --- */

/* WHY a fallback exists at all. On the X79-VG4 a USB keyboard (24ae:4056)
 * enumerated, bound and armed both endpoints, the EHCI schedule showed no
 * error bits, and still no interrupt transfer ever completed with data. A
 * keyboard that cannot be heard on the interrupt pipe can still be asked, over
 * the control pipe that enumeration just proved works, what keys are down
 * (HID 1.11 7.2.1 GET_REPORT). That makes the keyboard usable while the
 * interrupt pipe stays unexplained, and the panel row that says "poll ctl"
 * makes the situation unmistakable on the next photograph.
 *
 * The cost is bounded on purpose. It starts only after 3 s of interrupt
 * silence on a keyboard-role interface, polls at the 16 ms the core's timer
 * can give, and ends permanently the moment the interrupt endpoint delivers a
 * single report. Linux never does this to a boot keyboard (usbhid_parse sets
 * HID_QUIRK_NOGET on them), so it is a probe first: one GET_REPORT, and only a
 * decodable answer starts the polling; a STALL or a timeout retires the
 * interface as FAILED and nothing is ever sent again. */

static uint64_t hid_service(struct usb_device *d, int ifno);

/* WHY: one place that changes the state and mirrors it to the panel, so the
 * two cannot disagree. Returns whether this call made the change. */
static int hid_ctl_transition(struct hid_dev *h, uint8_t from, uint8_t to)
{
    uint8_t expected = from;
    if (!__atomic_compare_exchange_n(&h->ctl_state, &expected, to, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return 0;
    h->diag->ctl_state = to;
    return 1;
}

/* WHY: the first state is set exactly once, at the end of the probe, before any
 * other context can see this interface. */
static void hid_ctl_enter(struct hid_dev *h, uint8_t state)
{
    __atomic_store_n(&h->ctl_state, state, __ATOMIC_RELEASE);
    h->diag->ctl_state = state;
}

/* WHY: the interrupt endpoint works, so nothing the control pipe could add is
 * worth its cost, and a late GET_REPORT answer must never be mistaken for a
 * newer state than the interrupt report just seen. Called under the IRQ
 * callback lock for the first report; the compare-and-swap makes it safe
 * against hid_service() promoting the same interface at that instant. */
static void hid_ctl_retire(struct hid_dev *h)
{
#ifndef USB_HID_NEGCTL_FALLBACK_KEEPS_RUNNING
    /* Every report of every interface passes here, mice at up to 1 kHz: only a
     * keyboard still waiting or polling pays for the atomic exchange. */
    uint8_t state = __atomic_load_n(&h->ctl_state, __ATOMIC_ACQUIRE);
    if (state != HID_DIAG_CTL_WAIT && state != HID_DIAG_CTL_POLL)
        return;
    (void)hid_ctl_transition(h, state, HID_DIAG_CTL_RETIRED);
#else
    (void)h;
#endif
}

/* WHY: GET_REPORT must name a report ID and a length the device's own
 * descriptor defines, so decide them once from it. Only reports that carry
 * Keyboard-page input are polled: a consumer-control report on the same
 * interface would decode to nothing here. With no descriptor the interface is
 * a boot keyboard and its single input report is the 8-byte Appendix B.1 one. */
static void hid_ctl_collect(struct hid_dev *h)
{
    h->ctl_nreports = 0;
    if (!(h->role & ROLE_KBD))
        return;
    if (!h->generic) {
        h->ctl[0].id = 0;
        h->ctl[0].len = 8;
        h->ctl_nreports = 1;
        return;
    }
    for (int slot = 0; slot < h->rd.nreports && h->ctl_nreports < HID_MAX_REPORTS; slot++) {
        int has_keys = 0;
        for (int i = 0; i < h->rd.nfields && !has_keys; i++) {
            const struct hid_field *f = &h->rd.f[i];
            has_keys = f->is_input && f->usage_page == HID_PAGE_KEYBOARD &&
                       f->report_id == h->rd.rep[slot].id;
        }
        unsigned bytes = ((unsigned)h->rd.rep[slot].in_bits + 7u) / 8u + (h->rd.uses_report_ids ? 1u : 0u);
        if (!has_keys || !bytes || bytes > HID_CTL_MAX_REPORT)
            continue;
        h->ctl[h->ctl_nreports].id = h->rd.rep[slot].id;
        h->ctl[h->ctl_nreports].len = (uint8_t)bytes;
        h->ctl_nreports++;
    }
}

/* WHY: a STALL-free answer of the wrong shape (an all-zero blob, or the wrong
 * report) must not be adopted as an input path. The same decoder that will
 * consume the polled bytes judges them first, without side effects. */
static int hid_keyboard_report_valid(const struct hid_dev *h, const uint8_t *report, int length)
{
    if (h->generic) {
        struct hid_kbd_state keys;
        return hid_decode_keyboard(&h->rd, report, length, &keys) > 0;
    }
    return length >= 8;
}

/* WHY: a sibling interface of the same device that already delivered an
 * interrupt report proves the transport works; a silent keyboard-role
 * interface next to a working one is almost always just unused (a media-key or
 * NKRO interface nobody has pressed), and must not start a 60 Hz poll. */
static int hid_sibling_delivers(const struct usb_device *d, const struct hid_dev *self)
{
    for (int i = 0; i < d->cfg.n_if; i++) {
        const struct usb_driver *driver = d->binding[i].drv;
        const struct hid_dev *other = d->binding[i].drvdata;
        if (!driver || driver->service != hid_service || !other || other == self)
            continue;
        if (__atomic_load_n(&other->reports, __ATOMIC_ACQUIRE))
            return 1;
    }
    return 0;
}

/* WHY: this is where a polled report becomes keystrokes, and it must produce
 * exactly what the interrupt path would have. GET_REPORT returns the CURRENT
 * state every time, so the same bytes come back on every poll; feeding them all
 * would look like a held key to handle_keyboard()'s repeat logic and type
 * auto-repeat at 62 Hz. A changed report is fed at once (an edge). An unchanged
 * non-empty one is fed once per 96 ms, which is the re-report SET_IDLE(24)
 * would have produced, so a held key starts repeating after the same 4 ticks.
 * An unchanged empty report says nothing and is dropped. The caller holds the
 * controller's callback lock, the lock hid_poll() runs under, so history[] is
 * never touched by both paths at once. */
static void hid_ctl_feed(struct hid_dev *h, struct hid_ctl_report *r, const uint8_t *report,
                         int length, uint64_t now)
{
    int changed = length != r->prev_len;
    int nonzero = 0;
    for (int i = 0; i < length; i++) {
        if (!changed && report[i] != r->prev[i])
            changed = 1;
        if (report[i])
            nonzero = 1;
    }
    int idle_rereport = !changed && nonzero && now - r->repeat_ns >= HID_CTL_REPEAT_NS;
#ifdef USB_HID_NEGCTL_FALLBACK_DUPLICATES
    changed = 1;
#endif
    if (!changed && !idle_rereport)
        return;
    r->repeat_ns = now;
    r->prev_len = (uint8_t)length;
    for (int i = 0; i < length; i++)
        r->prev[i] = report[i];
    handle_keyboard(h, report, length);
    h->diag->ctl_fed++;
}

/* WHY: the one place the fallback talks to the device. It runs on the kworker
 * with the device pinned by the core (usb_core.c service_work), so it may issue
 * sleeping control transfers and keep `h` across them; it takes the controller
 * callback lock only around feeding the decoder, never across a transfer, and
 * re-checks the state under it so a report that arrived on the interrupt pipe
 * during the transfer wins and the stale control answer is discarded.
 * Returns when the core should call again, or 0 to stop. */
static uint64_t hid_service(struct usb_device *d, int ifno)
{
    struct hid_dev *h = d->binding[ifno].drvdata;
    if (!h)
        return 0;
    uint8_t state = __atomic_load_n(&h->ctl_state, __ATOMIC_ACQUIRE);
    if (state != HID_DIAG_CTL_WAIT && state != HID_DIAG_CTL_POLL)
        return 0;
    if (hid_sibling_delivers(d, h)) {
        hid_ctl_retire(h);
        return 0;
    }

    uint64_t now = time_mono_ns();
    if (state == HID_DIAG_CTL_WAIT) {
        /* The clock may not have been ready when the endpoint was armed. */
        if (!h->armed_ns)
            h->armed_ns = now;
        if (now < h->armed_ns + HID_CTL_SILENCE_NS)
            return h->armed_ns + HID_CTL_SILENCE_NS - now;
    }

    int answered = 0;
    for (unsigned slot = 0; slot < h->ctl_nreports; slot++) {
        struct hid_ctl_report *r = &h->ctl[slot];
        uint8_t report[HID_CTL_MAX_REPORT];
        memset(report, 0, sizeof report);
        int length = hid_get_report(d, h->ifnum, r->id, report, r->len);
        int valid = length > 0 && hid_keyboard_report_valid(h, report, length);
        h->diag->ctl_polls++;
        h->diag->ctl_rc = valid ? HID_DIAG_REQ_OK : HID_DIAG_REQ_FAILED;
        h->diag->ctl_len = (uint8_t)(length > 0 ? length : 0);
        for (int i = 0; i < 8; i++)
            h->diag->ctl_first[i] = length > i ? report[i] : 0;
        if (!valid) {
            h->diag->ctl_fail++;
            continue;
        }
        answered = 1;
        now = time_mono_ns();
        int promoted = 0;
        {
            /* Serial output stays outside the IRQ-off lock. */
            IO_GUARD(&d->hc->callbacks);
            promoted = state == HID_DIAG_CTL_WAIT &&
                       hid_ctl_transition(h, HID_DIAG_CTL_WAIT, HID_DIAG_CTL_POLL);
            if (__atomic_load_n(&h->ctl_state, __ATOMIC_ACQUIRE) == HID_DIAG_CTL_POLL)
                hid_ctl_feed(h, r, report, length, now);
        }
        if (promoted)
            kprintf("[hid] if%d interrupt pipe silent for 3 s; polling GET_REPORT\n", h->ifnum);
    }

    if (answered) {
        h->ctl_fail_run = 0;
    } else if (state == HID_DIAG_CTL_WAIT || ++h->ctl_fail_run >= HID_CTL_GIVE_UP) {
        if (hid_ctl_transition(h, state, HID_DIAG_CTL_FAILED))
            kprintf("[hid] if%d GET_REPORT fallback unusable; staying on the interrupt pipe\n",
                    h->ifnum);
    }
    state = __atomic_load_n(&h->ctl_state, __ATOMIC_ACQUIRE);
    return state == HID_DIAG_CTL_POLL ? HID_CTL_POLL_NS : 0;
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
    h->diag = &hid_diag_spare;

    /* Everything the probe learns or sends, kept locally until a diag slot is
     * taken at the end: a failed probe must not consume one. */
    struct usb_hid_diag probe_log;
    memset(&probe_log, 0, sizeof probe_log);
    probe_log.subclass = it->if_subclass;
    probe_log.protocol = it->if_proto;
    probe_log.max_packet = ep->max_packet;
    probe_log.interval = ep->interval;

    /* Parse the report descriptor if the interface declared one. */
    if (it->has_hid && it->hid_report_len > 0 && it->hid_report_len <= 4096) {
        uint8_t *rd = kmalloc(it->hid_report_len);
        if (rd) {
            int n = hid_get_report_desc(d, it->num, rd, it->hid_report_len);
            probe_log.rdesc_rc = n > 0 ? HID_DIAG_REQ_OK : HID_DIAG_REQ_FAILED;
            probe_log.rdesc_len = (uint16_t)(n > 0 ? n : 0);
            if (n > 0 && hid_parse_report_desc(rd, n, &h->rd) == 0) {
                h->have_rd = 1;
                kprintf("USB_HID_RD if=%d bytes=%d fields=%d reports=%d ids=%d\n", it->num, n,
                        h->rd.nfields, h->rd.nreports, h->rd.uses_report_ids);
            } else {
                kprintf("[hid] if%d: report descriptor %s\n", it->num,
                        n > 0 ? "did not parse" : "could not be read");
                /* the bytes themselves, so a photo of the screen is enough to
                 * reproduce the refusal in the host test */
                for (int o = 0; o < n && o < 192; o += 24) {
                    static const char hex[] = "0123456789abcdef";
                    char line[24 * 3 + 1];
                    int k = 0;
                    for (int j = o; j < n && j < o + 24; j++) {
                        line[k++] = hex[rd[j] >> 4];
                        line[k++] = hex[rd[j] & 15];
                        line[k++] = ' ';
                    }
                    line[k] = 0;
                    kprintf("[hid] if%d rd+%03d %s\n", it->num, o, line);
                }
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
        /* HID 1.11 sections 4.4/7.1.1 make the interface the descriptor and
         * failure boundary: a report-only multimedia sibling may be unknown
         * while if0 already supplies boot input. Include VID:PID and the wire
         * interface number so two composite devices behind one hub can be
         * diagnosed separately; do not remove their other bindings here.
         * https://www.usb.org/sites/default/files/hid1_11.pdf
         * Model evidence only; not verified on X79-VG4 hardware. */
        kprintf("[usb] %04x:%04x if%d report desc unsupported\n",
                d->dd.vendor, d->dd.product, it->num);
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
    probe_log.decoder = h->generic ? HID_DIAG_DECODE_DESCRIPTOR : HID_DIAG_DECODE_BOOT;
    if (!h->generic) {
        /* Fallback. SET_PROTOCOL(boot) is what makes the fixed layout a
         * guarantee rather than a hope, and it only works on a boot-capable
         * interface -- so if this is not one, we have nothing to decode with. */
        if (it->if_subclass != USB_HID_SUB_BOOT) {
            kprintf("[hid] if%d: no usable report descriptor and no boot protocol\n", it->num);
            kfree(h);
            return -1;
        }
        probe_log.proto_sent = HID_DIAG_PROTO_BOOT;
        if (hid_set_protocol(d, it->num, HID_PROTO_BOOT) < 0) {
            /* The former "assuming boot layout" on a refused request silently
             * interpreted unverified bytes as keys. A STALL gives no layout
             * guarantee; leave this interface unbound. */
            kprintf("[hid] if%d: SET_PROTOCOL(boot) refused\n", it->num);
            kfree(h);
            return -1;
        }
        probe_log.proto_rc = HID_DIAG_REQ_OK;
    }
    /* A parsed descriptor sends NO SET_PROTOCOL, as Linux's usbhid does: a
     * device is in report protocol after reset (HID 1.11 7.2.6 says it defaults
     * there when initialised) and usbhid relies on that. The former
     * SET_PROTOCOL(report) to every boot-subclass interface is dropped to match,
     * one class request fewer in the sequence a silent keyboard has to survive.
     * Non-boot interfaces never received SET_PROTOCOL at all: the request is
     * only required of boot-subclass devices (HID 1.11 7.2.6). */

    if ((h->role & HID_ROLE_TOUCH) && hid_touchpad_configure(d, it->num, &h->rd) < 0) {
        kprintf("[hid] if%d: touchpad mode negotiation refused\n", it->num);
        kfree(h);
        return -1;
    }

    /* HID 1.11 7.2.4/7.2.6: descriptor -> protocol -> touchpad negotiation ->
     * SET_IDLE -> arm. A refused idle request is advisory, but must be visible:
     * no reports from a NAK-only keyboard then does not prove a dead HC.
     * Not verified on X79-VG4 hardware. */
#ifdef EHCI_NEGCTL_ARM_FIRST
    (void)usb_int_in_arm(d, h->ep_addr);
#endif
    int idle_result = hid_set_idle(d, it->num, (h->role & ROLE_KBD) ? 24 : 0);
    kprintf("[hid] if%d SET_IDLE %s\n", it->num, idle_result < 0 ? "refused" : "ok");
    probe_log.idle_rc = idle_result < 0 ? HID_DIAG_REQ_FAILED : HID_DIAG_REQ_OK;
    probe_log.idle_units = (h->role & ROLE_KBD) ? 24 : 0;

    /* After every other probe step and before the first interrupt transfer is
     * possible, as in Linux (the endpoint is submitted when the input device
     * is opened, which is after usbhid_start() returned). */
    (void)hid_send_led_report(d, h, it, &probe_log);
    hid_ctl_collect(h);

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

    h->diag = &hid_diag_spare;
    for (int k = 0; k < HID_DIAG_SLOTS; k++)
        if (!hid_diag_used[k]) {
            hid_diag_slots[k] = probe_log;
            h->diag = &hid_diag_slots[k];
            __atomic_store_n(&hid_diag_used[k], 1, __ATOMIC_RELEASE);
            break;
        }
    h->diag->vid = d->dd.vendor; h->diag->pid = d->dd.product;
    h->diag->ifnum = (uint8_t)it->num;
    h->diag->role = (uint8_t)(((h->role & ROLE_KBD) ? 1 : 0) | ((h->role & ROLE_MOUSE) ? 2 : 0));

    /* Read once, after everything else, so the answer describes the state the
     * device is in while it is being asked for input. A device that reports 0
     * here is not configured and no endpoint of it can ever deliver. */
    uint8_t configuration = 0;
    int configuration_result = hid_get_configuration(d, &configuration);
    h->diag->cfg_rc = configuration_result < 0 ? HID_DIAG_REQ_FAILED : HID_DIAG_REQ_OK;
    h->diag->cfg_value = configuration;
    kprintf("[hid] if%d GET_CONFIGURATION %s value=%u\n", it->num,
            configuration_result < 0 ? "refused" : "ok", configuration);

    h->armed_ns = time_ready() ? time_mono_ns() : 0;
    if (h->ctl_nreports)
        hid_ctl_enter(h, HID_DIAG_CTL_WAIT);
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
        h->diag->reports++;
        hid_ctl_retire(h);
        h->diag->last_len = (uint8_t)(n > 255 ? 255 : n);
        for (int i = 0; i < 8; i++) h->diag->last[i] = i < n ? buf[i] : 0;
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
     * have told us so is the one we are no longer collecting. EHCI owns STALL
     * recovery on its worker; do not issue synchronous CLEAR_FEATURE here on
     * the IRQ stack or change xHCI's existing completion behavior. */
    usb_int_in_arm(d, h->ep_addr);
}

static void hid_remove(struct usb_device *d, int ifno)
{
    struct hid_dev *h = d->binding[ifno].drvdata;
    if (h) {
        for (int k = 0; k < HID_DIAG_SLOTS; k++)
            if (h->diag == &hid_diag_slots[k]) __atomic_store_n(&hid_diag_used[k], 0, __ATOMIC_RELEASE);
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
    .service = hid_service,
};

void usb_hid_register(void)
{
    usb_register_driver(&hid_driver);
}
