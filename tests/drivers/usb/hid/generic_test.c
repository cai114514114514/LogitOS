/* Protocol fixture, not a hardware claim: real usb_bind.c + usb_hid.c +
 * hid_report.c run through fake control/interrupt transport into WM spies.
 * Reuse the established HID descriptor fixtures and their regression suite. */
#define main usb_existing_regressions
#include "../../../unit/usb_hid_test.c"
#include "hid/generic.h"
#include "fixtures.h"
#undef main
#include <stdlib.h>
#include <stdarg.h>
#include "usb.h"

static const struct usb_driver *registered;
static const uint8_t *descriptors[USB_MAX_IF];
static int lengths[USB_MAX_IF], pending[16], allocated, configured, protocols;
static int refuse_protocol;
static int feature_active;
static int feature_fault;
static int feature_writes;
static int feature_arms;
static uint8_t feature_mode;
static uint8_t packets[16][128];
static int keys_seen, last_key, last_key_mods, mouse_seen, mx, my, ml, mm, last_wheel;
unsigned long usb_reports_total, usb_keys_total, usb_motion_total;

void usb_hid_register(void);
void usb_register_driver(const struct usb_driver *drv)
{
    registered = drv;
}
void *kmalloc(size_t n)
{
    void *p = calloc(1, n);
    if (p)
        allocated++;
    return p;
}
void kfree(void *p)
{
    if (p) {
        allocated--;
        free(p);
    }
}
void kprintf(const char *fmt, ...)
{
    (void)fmt;
}
uint32_t fb_width(void)
{
    return 1280;
}
uint32_t fb_height(void)
{
    return 800;
}
void wm_key(int k)
{
    keys_seen++;
    last_key = k;
    last_key_mods = usb_hid_mods();
}
void wm_mouse_event(int x, int y, int l, int r, int m, int wheel)
{
    (void)r;
    (void)m;
    last_wheel = wheel;
    mouse_seen++;
    mx = x;
    my = y;
    ml = l;
    mm = usb_hid_mods();
}
int usb_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                void *buf, uint16_t len)
{
    (void)d;
    if (feature_active && (req == HID_REQ_GET_REPORT || req == HID_REQ_SET_REPORT)) {
        uint8_t *packet = buf;
        checki(idx, 0, "Feature report targets the HID interface");
        if (req == HID_REQ_GET_REPORT) {
            checki(rt, 0xa1, "GET_REPORT uses IN class interface request");
            if (val == 0x0305 && len == 2) {
                packet[0] = 5;
                packet[1] = feature_fault == 5 ? 3 : 2;
                return 2;
            }
            if (val != 0x0306 || len != 3)
                return -1;
            packet[0] = feature_fault == 2 ? 7 : 6;
            packet[1] = (uint8_t)(5 | (feature_mode << 3));
            packet[2] = 0xa7;
            return feature_fault == 1 ? 2 : 3;
        }
        checki(rt, 0x21, "SET_REPORT uses OUT class interface request");
        checki(val, 0x0306, "SET_REPORT selects Feature type and actual report ID");
        if (len != 3)
            return -1;
        checki(packet[0], 6, "SET_REPORT includes the report ID prefix");
        checki(packet[1] & 7, 5, "Feature write preserves leading unrelated bits");
        checki(packet[2], 0xa7, "Feature write preserves trailing vendor byte");
        feature_writes++;
        if (feature_fault == 3)
            return -1;
        if (feature_fault != 4)
            feature_mode = packet[1] >> 3;
        return 3;
    }
    if (req == USB_REQ_GET_DESCRIPTOR && val == (USB_DT_HID_REPORT << 8)) {
        if (idx >= USB_MAX_IF || !descriptors[idx] || len < lengths[idx])
            return -1;
        memcpy(buf, descriptors[idx], lengths[idx]);
        return lengths[idx];
    }
    if (req == HID_REQ_SET_PROTOCOL) {
        protocols++;
        if (refuse_protocol)
            return -1;
        checki(val, HID_PROTO_REPORT, "boot-capable parsed HID explicitly selects report protocol");
    }
    return 0;
}
int usb_configure_interface(struct usb_device *d, const struct usb_interface *it)
{
    (void)d;
    (void)it;
    configured++;
    return 0;
}
int usb_int_in_arm(struct usb_device *d, uint8_t ep)
{
    (void)d;
    (void)ep;
    if (feature_active) {
        feature_arms++;
        checki(feature_mode, 3, "input endpoint armed only after mode readback");
    }
    return 0;
}
int usb_int_in_poll(struct usb_device *d, uint8_t ep, uint8_t **buf)
{
    (void)d;
    ep &= 15;
    if (!pending[ep])
        return -1;
    int n = pending[ep];
    pending[ep] = 0;
    *buf = packets[ep];
    return n;
}
static void iface(struct usb_device *d, int i, const uint8_t *rd, int len, int boot, int proto)
{
    descriptors[i] = rd;
    lengths[i] = len;
    struct usb_interface *it = &d->cfg.iface[i];
    it->num = i;
    it->if_class = USB_CLASS_HID;
    it->if_subclass = boot;
    it->if_proto = proto;
    it->n_ep = 1;
    it->has_hid = 1;
    it->hid_report_len = len;
    it->ep[0] = (struct usb_endpoint){
        .addr = 0x81 + i, .attr = USB_XFER_INT, .max_packet = 64, .interval = 10};
    d->cfg.n_if = i + 1;
}
static void inject(struct usb_device *d, int endpoint, const uint8_t *p, int len)
{
    memcpy(packets[endpoint], p, len);
    pending[endpoint] = len;
    usb_poll_interfaces(d);
}
static unsigned generic_checks;
static void gcheck(int cond, const char *what)
{
    generic_checks++;
    check(cond, what);
}
static void gchecki(long long got, long long want, const char *what)
{
    generic_checks++;
    checki(got, want, what);
}
#define check gcheck
#define checki gchecki
static void gamepad(void)
{
    struct usb_device d = {.used = 1};
    iface(&d, 0, gamepad_rd, sizeof gamepad_rd, 0, 0);
    checki(usb_bind_interfaces(&d, &registered, 1), 1,
           "generic gamepad binds through class driver");
    int k0 = keys_seen, m0 = mouse_seen;
    uint8_t axes[] = {1, 0, 0, 8}, buttons[] = {2, 1};
    inject(&d, 1, buttons, sizeof buttons);
    checki(last_key, ' ', "gamepad button 1 reaches WM Space consumer");
    checki(usb_hid_key_held(' '), 1, "gamepad button feeds physical held query");
    inject(&d, 1, axes, sizeof axes);
    checki(usb_hid_key_held(' '), 1, "axes report retains separate button report");
    checki(keys_seen - k0, 1, "centered sticks and null hat generate no keys");
    axes[1] = 127;
    inject(&d, 1, axes, sizeof axes);
    checki(usb_hid_key_held(KEY_RIGHT), 1, "stick reaches existing game arrow consumer");
    axes[1] = 0;
    axes[3] = 7;
    inject(&d, 1, axes, sizeof axes);
    checki(usb_hid_key_held(KEY_RIGHT), 0, "returning stick releases direction");
    checki(usb_hid_key_held(KEY_LEFT), 1, "diagonal hat left");
    checki(usb_hid_key_held(KEY_UP), 1, "diagonal hat up");
    checki(mouse_seen, m0, "gamepad cannot enter mouse decoder");
    int held_before = keys_seen;
    inject(&d, 1, axes, sizeof axes);
    checki(keys_seen, held_before, "held pad reports do not generate repeated key edges");
    usb_remove_interfaces(&d);
    checki(usb_hid_key_held(' '), 0, "unplug releases pad buttons");
    checki(usb_hid_key_held(KEY_LEFT), 0, "unplug releases pad hat");
    checki(allocated, 0, "pad binding freed");
}
static void contact(uint8_t *p, int slot, int id, int x, int y, int flags)
{
    unsigned n = 1 + 6 * slot;
    p[n] = flags;
    p[n + 1] = id;
    p[n + 2] = x;
    p[n + 3] = x >> 8;
    p[n + 4] = y;
    p[n + 5] = y >> 8;
}
static void touchpad(void)
{
    struct usb_device d = {.used = 1};
    iface(&d, 0, touchpad_rd, sizeof touchpad_rd, 0, 0);
    checki(usb_bind_interfaces(&d, &registered, 1), 1,
           "generic touchpad binds through class driver");
    uint8_t frame[14] = {3}, button[] = {4, 1};
    frame[13] = 1;
    contact(frame, 0, 10, 1000, 1000, 3);
    contact(frame, 1, 20, 0, 0, 0);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 640, "initial contact anchors without absolute jump");
    checki(my, 400, "initial Y anchor");
    contact(frame, 0, 10, 1512, 1000, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 656, "single contact relative motion reaches WM");
    checki(ml, 0, "finger tip alone is not a click");
    inject(&d, 1, button, sizeof button);
    checki(ml, 1, "touchpad physical click reaches WM");
    contact(frame, 0, 10, 1768, 1000, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 664, "button-only report preserves contact anchor");
    checki(ml, 1, "contact report preserves separate held click");
    contact(frame, 0, 99, 20000, 1000, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 664, "replacement contact ID reanchors without teleport");
    frame[13] = 2;
    contact(frame, 1, 20, 21000, 1000, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(last_wheel, 0, "second finger transition does not scroll");
    contact(frame, 0, 99, 20000, 2024, 3);
    contact(frame, 1, 20, 21000, 2024, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(last_wheel, 2, "two finger movement reaches WM positive down scroll");
    checki(mx, 664, "two fingers do not move pointer");
    contact(frame, 0, 20, 21000, 2024, 3);
    contact(frame, 1, 99, 20000, 2024, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(last_wheel, 0, "contact slot reorder keeps ID-stable gesture");
    frame[13] = 0;
    inject(&d, 1, frame, sizeof frame);
    frame[13] = 1;
    contact(frame, 0, 99, 1000, 1000, 3);
    contact(frame, 1, 20, 0, 0, 0);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 664, "zero count clears old contact anchor");
    contact(frame, 0, 99, 20000, 1000, 1);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 664, "confidence false excludes palm contact");
    contact(frame, 0, 99, 30000, 1000, 3);
    inject(&d, 1, frame, sizeof frame);
    checki(mx, 664, "confidence recovery reanchors");
    inject(&d, 1, frame, 5);
    checki(ml, 0, "truncated touch report releases old drag");
    inject(&d, 1, button, sizeof button);
    checki(ml, 1, "new click after malformed report");
    usb_remove_interfaces(&d);
    checki(ml, 0, "unplug touchpad sends button release to WM");
    checki(allocated, 0, "touch state freed");
}
static void decode_boundaries(void)
{
    struct hid_desc d;
    struct hid_gamepad_state s = {0};
    struct hid_gamepad state;
    checki(hid_parse_report_desc(gamepad_rd, sizeof gamepad_rd, &d), 0, "pad descriptor parses");
    check(!hid_looks_like_mouse(&d), "Application collection excludes pad from mouse roles");
    uint8_t p[] = {1, 0x81, 0x7f, 8};
    checki(hid_gamepad_decode(&d, p, sizeof p, &s, &state), 1, "gamepad decodes signed extrema");
    checki(state.axes[0], -32767, "minimum stick normalized");
    checki(state.axes[1], 32767, "maximum stick normalized");
    checki(state.hat, -1, "out of range null hat neutral");
    p[0] = 9;
    checki(hid_gamepad_decode(&d, p, sizeof p, &s, &state), 0, "unknown ID is ignored");
    p[0] = 1;
    checki(hid_gamepad_decode(&d, p, 2, &s, &state), -1, "short axes rejected");
    checki(hid_parse_report_desc(touchpad_rd, sizeof touchpad_rd, &d), 0,
           "touch descriptor parses");
    struct hid_touch_frame f;
    uint8_t t[14] = {3};
    t[13] = 2;
    contact(t, 0, 7, 1000, 1000, 3);
    contact(t, 1, 7, 1000, 1000, 3);
    checki(hid_touch_decode(&d, t, sizeof t, &f), -1, "duplicate active contact IDs rejected");
    t[13] = 3;
    checki(hid_touch_decode(&d, t, sizeof t, &f), -1,
           "hybrid frame exceeds available contacts and is refused");
    /* Full-width local usages carry a page independent of current global page. */
    const uint8_t full[] = {5,    1, 9,    5, 0xa1, 1, 0x0b, 1, 0,    9, 0,
                            0x15, 0, 0x25, 1, 0x75, 1, 0x95, 1, 0x81, 2, 0xc0};
    checki(hid_parse_report_desc(full, sizeof full, &d), 0, "extended usage parses");
    checki(d.f[0].usage_page, 9, "extended local usage page preserved");
    checki(d.f[0].usage, 1, "extended usage value preserved");
    const uint8_t wrapped[] = {5, 1, 9, 0x30, 0x77, 8, 1, 0, 0, 0x95, 1, 0x81, 2};
    checki(hid_parse_report_desc(wrapped, sizeof wrapped, &d), -1,
           "large report size rejected before integer narrowing");
    const uint8_t unclosed[] = {5, 1, 9, 2, 0xa1, 1, 9, 0x30, 0x75, 8, 0x95, 1, 0x81, 2};
    checki(hid_parse_report_desc(unclosed, sizeof unclosed, &d), -1,
           "unclosed collection rejected");
    const uint8_t mixed[] = {5, 1, 9, 0x30, 0x75, 8, 0x95, 1, 0x81, 2, 0x85, 1, 9, 0x31, 0x81, 2};
    checki(hid_parse_report_desc(mixed, sizeof mixed, &d), -1,
           "mixed implicit and explicit report IDs rejected");
}
static void bounded_reports(void)
{
    struct hid_desc desc;
    checki(hid_parse_report_desc(gamepad_rd, sizeof gamepad_rd, &desc), 0,
           "truncation fixture parses");
    struct hid_gamepad_state state = {0}, before;
    struct hid_gamepad out;
    uint8_t p[] = {1, 127, 0, 8};
    checki(hid_gamepad_decode(&desc, p, sizeof p, &state, &out), 1,
           "valid input establishes saved state");
    before = state;
    for (int i = 0; i < 4; i++) {
        checki(hid_gamepad_decode(&desc, p, i, &state, &out), -1,
               "every truncated game report rejected");
        check(!memcmp(&before, &state, sizeof state),
              "truncated report does not partially commit game state");
    }
    struct usb_device d = {.used = 1};
    iface(&d, 0, touchpad_rd, sizeof touchpad_rd, 0, 0);
    d.cfg.iface[0].ep[0].max_packet = 8;
    checki(usb_bind_interfaces(&d, &registered, 1), 0,
           "report longer than transport packet remains unbound");
    checki(allocated, 0, "oversized report probe frees state");
    /* Two USB pointers with independent held buttons. Removing the first must
     * publish a release while preserving the other interface's held level. */
    struct usb_device both = {.used = 1};
    iface(&both, 0, touchpad_rd, sizeof touchpad_rd, 0, 0);
    iface(&both, 1, touchpad_rd, sizeof touchpad_rd, 0, 0);
    checki(usb_bind_interfaces(&both, &registered, 1), 2, "two touchpad interfaces bind");
    uint8_t button[] = {4, 1};
    inject(&both, 1, button, 2);
    inject(&both, 2, button, 2);
    registered->remove(&both, 0);
    checki(ml, 1, "removing one pad preserves other held click");
    registered->remove(&both, 1);
    checki(ml, 0, "removing last pad releases click");
    checki(allocated, 0, "both pointer states freed");
}
static void feature_negotiation(void)
{
    /* Literal descriptor deliberately shares report ID 6 across all three
     * report types. Feature bit offsets must exclude Input and Output bits. */
    static const uint8_t features[] = {
        5, 0x0d, 9, 5, 0xa1, 1, 0x85, 5,
        9, 0x55, 0x15, 0, 0x25, 10, 0x75, 8, 0x95, 1, 0xb1, 2, 0xc0,
        5, 0x0d, 9, 0x0e, 0xa1, 1, 0x85, 6,
        0x75, 3, 0x95, 1, 0xb1, 3, 0x81, 3,
        0x75, 8, 0x95, 9, 0x91, 3,
        9, 0x52, 0x15, 0, 0x25, 10, 0x75, 5, 0x95, 1, 0xb1, 2,
        0x06, 0x00, 0xff, 9, 1, 0x75, 8, 0xb1, 2, 0xc0,
        /* Certification payload: layout matters, but 256 vendor bytes must
         * not exhaust the actionable field table or become Input fields. */
        5, 0x0d, 9, 5, 0xa1, 1, 0x85, 7, 0x06, 0, 0xff,
        9, 0xc5, 0x75, 8, 0x96, 0, 1, 0xb1, 2, 0xc0,
    };
    uint8_t descriptor[sizeof touchpad_rd + sizeof features];
    memcpy(descriptor, touchpad_rd, sizeof touchpad_rd);
    memcpy(descriptor + sizeof touchpad_rd, features, sizeof features);
    struct hid_desc parsed;
    checki(hid_parse_report_desc(descriptor, sizeof descriptor, &parsed), 0,
           "touchpad configuration Feature descriptor parses");
    checki(hid_report_in_bits(&parsed, 6), 3, "Feature layout does not advance Input cursor");
    checki(hid_report_feature_bits(&parsed, 6), 16,
           "Input and Output layout do not advance Feature cursor");
    checki(hid_report_feature_bits(&parsed, 7), 2048,
           "vendor certification Feature keeps its full bounded extent");
    checki(hid_report_in_bits(&parsed, 7), 0,
           "vendor Feature does not become an oversized interrupt report");

    feature_active = 1;
    feature_fault = 0;
    feature_mode = 0;
    feature_writes = 0;
    feature_arms = 0;
    struct usb_device device = {.used = 1};
    iface(&device, 0, descriptor, sizeof descriptor, 0, 0);
    int bound = usb_bind_interfaces(&device, &registered, 1);
    checki(bound, 1, "touchpad mode negotiated before input");
    if (bound) {
        checki(feature_writes, 1, "touchpad activation sends one mode request");
        checki(feature_arms, 1, "configured touchpad arms input once");
        uint8_t frame[14] = {3};
        frame[13] = 1;
        contact(frame, 0, 9, 1000, 1000, 3);
        inject(&device, 1, frame, sizeof frame);
        int initial_x = mx;
        contact(frame, 0, 9, 1512, 1000, 3);
        inject(&device, 1, frame, sizeof frame);
        checki(mx - initial_x, 16, "negotiated touchpad reports reach WM motion");
    }
    usb_remove_interfaces(&device);
    checki(allocated, 0, "negotiated touchpad removal releases allocations");

    const char *refusals[] = {
        "short Feature read refuses binding",
        "wrong Feature report ID refuses binding",
        "stalled Feature write refuses binding",
        "ignored mode write refuses binding",
        "hybrid contact capacity refuses mode switch",
    };
    for (feature_fault = 1; feature_fault <= 5; feature_fault++) {
        feature_mode = 0;
        feature_writes = 0;
        feature_arms = 0;
        device = (struct usb_device){.used = 1};
        iface(&device, 0, descriptor, sizeof descriptor, 0, 0);
        checki(usb_bind_interfaces(&device, &registered, 1), 0,
               refusals[feature_fault - 1]);
        checki(feature_arms, 0, "failed negotiation never arms input");
        checki(allocated, 0, "failed negotiation frees private state");
        if (feature_fault == 5)
            checki(feature_writes, 0, "unsupported hybrid frame never disables mouse mode");
        usb_remove_interfaces(&device);
    }
    feature_active = 0;
    feature_fault = 0;
    device = (struct usb_device){.used = 1};
    iface(&device, 0, features, sizeof features, 0, 0);
    checki(usb_bind_interfaces(&device, &registered, 1), 0,
           "Feature-only touchpad collection does not masquerade as input");
    checki(allocated, 0, "Feature-only interface refusal releases state");
}

int main(void)
{
    if (usb_existing_regressions())
        return 1;
    usb_hid_register();
    decode_boundaries();
    gamepad();
    touchpad();
    bounded_reports();
    feature_negotiation();
    if (failures) {
        printf("usb-hid-generic: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("usb-hid-generic: %u checks, 0 failed; parser + real class bind/poll/remove + input "
           "consumers\n",
           generic_checks);
    return 0;
}
