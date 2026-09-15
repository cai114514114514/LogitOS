#ifndef LOGIT_USB_HID_GENERIC_H
#define LOGIT_USB_HID_GENERIC_H
#include "../hid_report.h"
#define HID_CONTACTS 10
#define HID_AXES 8
#define HID_ROLE_GAMEPAD 4
#define HID_ROLE_TOUCH 8

/* One bounded snapshot per report ID: axes-only reports must not release
 * buttons belonging to another report. Values are normalized to -32767..32767. */
struct hid_gamepad {
    int32_t axes[HID_AXES];
    uint32_t buttons, button_mask;
    unsigned axis_mask;
    int hat, has_hat;
};
struct hid_gamepad_state {
    struct hid_gamepad reports[HID_MAX_REPORTS];
};
struct hid_contact {
    uint32_t id;
    int x, y;      /* normalized absolute coordinates, 0..65535 */
    unsigned seen; /* fields received within this Finger collection */
    unsigned tip;
    unsigned confidence;
};
struct hid_touch_frame {
    struct hid_contact contacts[HID_CONTACTS];
    unsigned count;        /* active, confident contacts after validation */
    unsigned has_contacts; /* false for a standalone physical-button report */
    unsigned button_mask;
    unsigned buttons;
    uint8_t report_id;
};
struct hid_touch_state {
    unsigned active; /* number of contacts in the previous gesture anchor */
    unsigned buttons;
    unsigned button_mask[HID_MAX_REPORTS];
    unsigned button_levels[HID_MAX_REPORTS];
    uint32_t ids[2];
    int x, y; /* previous gesture centroid */
    int residual_x, residual_y;
    int residual_wheel;
    uint8_t report_id;
};
struct hid_pointer_delta {
    int dx, dy, wheel;
    unsigned buttons;
};

unsigned hid_generic_roles(const struct hid_desc *descriptor);

/* 1 matching validated report, 0 unrelated, -1 malformed/unsupported frame.
 * Decoders commit no state on failure. No allocation or device callbacks. */
int hid_gamepad_decode(const struct hid_desc *descriptor, const uint8_t *packet, int length,
                       struct hid_gamepad_state *state, struct hid_gamepad *combined);
void hid_gamepad_keys(const struct hid_gamepad *gamepad, struct hid_kbd_state *keys);
int hid_touch_decode(const struct hid_desc *descriptor, const uint8_t *packet, int length,
                     struct hid_touch_frame *output);
void hid_touch_apply(struct hid_touch_state *state, const struct hid_touch_frame *frame,
                     int report_slot, struct hid_pointer_delta *motion);
#endif
