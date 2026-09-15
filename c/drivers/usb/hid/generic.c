#include "generic.h"

/* Wire definitions: USB-IF HID 1.11 sections 6.2.2.5/6.2.2.8 and Usage Tables
 * 1.3 Generic Desktop + Digitizers. No VID/PID or guessed vendor reports.
 * https://www.usb.org/sites/default/files/documents/hid1_11.pdf
 * https://www.usb.org/sites/default/files/hut1_3_0.pdf */
#define GAMEPAD_AXIS_MAX 32767
#define GAMEPAD_DEAD_ZONE 8192
#define TOUCH_COORDINATE_MAX 65535
#define TOUCH_UNITS_PER_PIXEL 64
#define TOUCH_UNITS_PER_SCROLL_STEP 1024

enum contact_fields {
    CONTACT_HAS_X = 1,
    CONTACT_HAS_Y = 2,
    CONTACT_HAS_TIP = 4,
    CONTACT_HAS_ID = 8,
    CONTACT_REQUIRED_FIELDS = CONTACT_HAS_X | CONTACT_HAS_Y | CONTACT_HAS_TIP | CONTACT_HAS_ID
};

enum keyboard_usage {
    KEY_USAGE_ENTER = 0x28,
    KEY_USAGE_ESCAPE = 0x29,
    KEY_USAGE_TAB = 0x2b,
    KEY_USAGE_SPACE = 0x2c,
    KEY_USAGE_RIGHT = 0x4f,
    KEY_USAGE_LEFT = 0x50,
    KEY_USAGE_DOWN = 0x51,
    KEY_USAGE_UP = 0x52
};

enum report_error { REPORT_INVALID = -1, REPORT_UNKNOWN = -2 };

struct report_view {
    const uint8_t *body;
    int body_bits;
    uint8_t id;
};

static void clear_bytes(void *pointer, unsigned long size)
{
    unsigned char *bytes = pointer;
    while (size--)
        *bytes++ = 0;
}

static int is_gamepad_field(const struct hid_field *field)
{
    return field->application == HID_APP_JOYSTICK || field->application == HID_APP_GAMEPAD;
}

static int is_touchpad_field(const struct hid_field *field)
{
    return field->application == HID_APP_TOUCHPAD;
}

unsigned hid_generic_roles(const struct hid_desc *descriptor)
{
    unsigned roles = 0;
    for (int i = 0; i < descriptor->nfields; i++) {
        const struct hid_field *field = &descriptor->f[i];
        if (!field->is_input)
            continue;
        if (is_gamepad_field(field))
            roles |= HID_ROLE_GAMEPAD;
        if (is_touchpad_field(field))
            roles |= HID_ROLE_TOUCH;
    }
    return roles;
}

/* Return the descriptor's report slot, or distinguish malformed input from an
 * unrelated report ID. Only malformed input should release a prior gesture. */
static int open_report(const struct hid_desc *descriptor, const uint8_t *packet, int length,
                       struct report_view *view)
{
    if (!descriptor || !packet || length <= 0 || length > HID_MAX_REPORT_BYTES)
        return REPORT_INVALID;

    int prefix_bytes = descriptor->uses_report_ids ? 1 : 0;
    view->id = prefix_bytes ? packet[0] : 0;
    view->body = packet + prefix_bytes;
    view->body_bits = (length - prefix_bytes) * 8;
    for (int slot = 0; slot < descriptor->nreports; slot++) {
        if (descriptor->rep[slot].id != view->id)
            continue;
        if (view->body_bits < descriptor->rep[slot].in_bits)
            return REPORT_INVALID;
        return slot;
    }
    return REPORT_UNKNOWN;
}

static int normalize_axis(const struct hid_field *field, int32_t value, int maximum,
                          int *normalized)
{
    if (field->lmax <= field->lmin || value < field->lmin || value > field->lmax)
        return -1;

    int64_t range = (int64_t)field->lmax - field->lmin;
    *normalized = (int)(((int64_t)value - field->lmin) * maximum / range);
    return 0;
}

static int decode_hat(const struct hid_field *field, int32_t value, int *hat)
{
    /* Hats are 4 or 8 ordered directions; out-of-range is neutral only when
     * the descriptor explicitly permits the HID Null State. */
    int64_t directions = (int64_t)field->lmax - field->lmin + 1;
    if (directions != 4 && directions != 8)
        return -1;
    if (value < field->lmin || value > field->lmax) {
        if (!(field->flags & HID_MAIN_NULL_STATE))
            return -1;
        *hat = -1;
    } else {
        int step = directions == 4 ? 2 : 1;
        *hat = (value - field->lmin) * step;
    }
    return 0;
}

static int decode_gamepad_field(const struct hid_field *field, int32_t value,
                                struct hid_gamepad *snapshot)
{
    if (field->usage_page == HID_PAGE_DESKTOP && field->usage >= HID_USAGE_X &&
        field->usage <= HID_USAGE_DIAL) {
        int axis = (int)field->usage - HID_USAGE_X;
        int normalized;
        if (field->flags & HID_MAIN_RELATIVE)
            return -1;
        if (normalize_axis(field, value, GAMEPAD_AXIS_MAX * 2, &normalized) < 0)
            return -1;
        snapshot->axes[axis] = normalized - GAMEPAD_AXIS_MAX;
        snapshot->axis_mask |= 1u << axis;
        return 1;
    }
    if (field->usage_page == HID_PAGE_DESKTOP && field->usage == HID_USAGE_HAT) {
        if (decode_hat(field, value, &snapshot->hat) < 0)
            return -1;
        snapshot->has_hat = 1;
        return 1;
    }
    if (field->usage_page == HID_PAGE_BUTTON && field->usage >= 1 && field->usage <= 32) {
        uint32_t mask = 1u << (field->usage - 1);
        snapshot->button_mask |= mask;
        if (value)
            snapshot->buttons |= mask;
        return 1;
    }
    return 0;
}

static void merge_gamepad_reports(const struct hid_gamepad_state *state, int report_count,
                                  struct hid_gamepad *combined)
{
    clear_bytes(combined, sizeof *combined);
    combined->hat = -1;
    for (int slot = 0; slot < report_count; slot++) {
        const struct hid_gamepad *snapshot = &state->reports[slot];
        combined->buttons |= snapshot->buttons;
        combined->button_mask |= snapshot->button_mask;
        for (int axis = 0; axis < HID_AXES; axis++) {
            if (snapshot->axis_mask & (1u << axis))
                combined->axes[axis] = snapshot->axes[axis];
        }
        combined->axis_mask |= snapshot->axis_mask;
        if (snapshot->has_hat) {
            combined->hat = snapshot->hat;
            combined->has_hat = 1;
        }
    }
}

int hid_gamepad_decode(const struct hid_desc *descriptor, const uint8_t *packet, int length,
                       struct hid_gamepad_state *state, struct hid_gamepad *combined)
{
    if (!state || !combined)
        return -1;
    struct report_view report;
    int slot = open_report(descriptor, packet, length, &report);
    if (slot < 0)
        return slot == REPORT_UNKNOWN ? 0 : -1;

    struct hid_gamepad snapshot = {.hat = -1};
    int matched = 0;
    for (int i = 0; i < descriptor->nfields; i++) {
        const struct hid_field *field = &descriptor->f[i];
        if (field->report_id != report.id || !is_gamepad_field(field) || !field->is_input ||
            !(field->flags & HID_MAIN_VARIABLE))
            continue;
        int32_t value = hid_extract_signed(report.body, report.body_bits, field, 0);
        int result = decode_gamepad_field(field, value, &snapshot);
        if (result < 0)
            return -1;
        matched |= result;
    }
    if (!matched)
        return 0;

    /* Commit only after every field validates. Replacing just this report's
     * snapshot preserves buttons from an independent report ID. */
    state->reports[slot] = snapshot;
    merge_gamepad_reports(state, descriptor->nreports, combined);
    return 1;
}

void hid_gamepad_keys(const struct hid_gamepad *gamepad, struct hid_kbd_state *keys)
{
    clear_bytes(keys, sizeof *keys);
    /* Existing games consume physical keyboard levels. This default mapping
     * makes generic pads usable there without pretending a gamepad syscall
     * exists: left stick/D-pad arrows, buttons 1..4 Space/Enter/Escape/Tab.
     * A quarter-range dead zone prevents resting analog noise from typing. */
    int up = 0, down = 0, left = 0, right = 0;
    if (gamepad->axis_mask & 1) {
        left = gamepad->axes[0] < -GAMEPAD_DEAD_ZONE;
        right = gamepad->axes[0] > GAMEPAD_DEAD_ZONE;
    }
    if (gamepad->axis_mask & 2) {
        up = gamepad->axes[1] < -GAMEPAD_DEAD_ZONE;
        down = gamepad->axes[1] > GAMEPAD_DEAD_ZONE;
    }
    int hat = gamepad->hat;
    if (gamepad->has_hat && hat >= 0) {
        up |= hat == 0 || hat == 1 || hat == 7;
        right |= hat >= 1 && hat <= 3;
        down |= hat >= 3 && hat <= 5;
        left |= hat >= 5 && hat <= 7;
    }
    if (up)
        keys->keys[keys->nkeys++] = KEY_USAGE_UP;
    if (down)
        keys->keys[keys->nkeys++] = KEY_USAGE_DOWN;
    if (left)
        keys->keys[keys->nkeys++] = KEY_USAGE_LEFT;
    if (right)
        keys->keys[keys->nkeys++] = KEY_USAGE_RIGHT;

    static const uint8_t action_keys[] = {KEY_USAGE_SPACE, KEY_USAGE_ENTER, KEY_USAGE_ESCAPE,
                                          KEY_USAGE_TAB};
    for (int button = 0; button < 4; button++) {
        if (gamepad->buttons & (1u << button))
            keys->keys[keys->nkeys++] = action_keys[button];
    }
}

static int is_contact_field(const struct hid_field *field)
{
    if (!field->contact)
        return 0;
    if (field->usage_page == HID_PAGE_DESKTOP)
        return field->usage == HID_USAGE_X || field->usage == HID_USAGE_Y;
    if (field->usage_page == HID_PAGE_DIGITIZER)
        return field->usage == HID_USAGE_TIP_SWITCH || field->usage == HID_USAGE_TOUCH_VALID ||
               field->usage == HID_USAGE_CONTACT_ID;
    return 0;
}

/* A collection number identifies the slot in this descriptor; Contact ID
 * identifies the physical finger across reports. They cannot be interchanged. */
static struct hid_contact *find_contact_slot(struct hid_touch_frame *frame, uint16_t *collections,
                                             unsigned *slot_count, uint16_t collection)
{
    for (unsigned slot = 0; slot < *slot_count; slot++) {
        if (collections[slot] == collection)
            return &frame->contacts[slot];
    }
    if (*slot_count == HID_CONTACTS)
        return 0;
    unsigned slot = (*slot_count)++;
    collections[slot] = collection;
    frame->contacts[slot].confidence = 1;
    return &frame->contacts[slot];
}

static int decode_contact_field(const struct hid_field *field, int32_t value,
                                struct hid_contact *contact)
{
    if (field->usage_page == HID_PAGE_DESKTOP) {
        int position;
        if (field->flags & HID_MAIN_RELATIVE)
            return -1;
        if (normalize_axis(field, value, TOUCH_COORDINATE_MAX, &position) < 0)
            return -1;
        if (field->usage == HID_USAGE_X) {
            contact->x = position;
            contact->seen |= CONTACT_HAS_X;
        } else {
            contact->y = position;
            contact->seen |= CONTACT_HAS_Y;
        }
    } else if (field->usage == HID_USAGE_TIP_SWITCH) {
        contact->tip = value != 0;
        contact->seen |= CONTACT_HAS_TIP;
    } else if (field->usage == HID_USAGE_TOUCH_VALID) {
        contact->confidence = value != 0;
    } else {
        if (value < 0)
            return -1;
        contact->id = (uint32_t)value;
        contact->seen |= CONTACT_HAS_ID;
    }
    return 0;
}

static int finish_touch_frame(struct hid_touch_frame *frame, unsigned slot_count,
                              int declared_contacts)
{
    /* Parallel/full-frame touch reports only. A Contact Count larger than the
     * available Finger collections is serial/hybrid framing, which requires
     * Scan Time reassembly; refusing it avoids mixing contacts across frames.
     * Zero count explicitly releases even if unused slots contain old data. */
    if (declared_contacts < -1 || declared_contacts > (int)slot_count)
        return -1;
    if (declared_contacts == 0)
        slot_count = 0;

    for (unsigned slot = 0; slot < slot_count; slot++) {
        struct hid_contact contact = frame->contacts[slot];
        if (!contact.tip || !contact.confidence)
            continue;
        if (contact.seen != CONTACT_REQUIRED_FIELDS)
            return -1;
        for (unsigned previous = 0; previous < frame->count; previous++) {
            if (frame->contacts[previous].id == contact.id)
                return -1;
        }
        frame->contacts[frame->count++] = contact;
    }
    if (declared_contacts >= 0 && frame->count > (unsigned)declared_contacts)
        return -1;
    return 0;
}

int hid_touch_decode(const struct hid_desc *descriptor, const uint8_t *packet, int length,
                     struct hid_touch_frame *output)
{
    if (!output)
        return -1;
    struct report_view report;
    int slot = open_report(descriptor, packet, length, &report);
    if (slot < 0)
        return slot == REPORT_UNKNOWN ? 0 : -1;

    struct hid_touch_frame frame = {.report_id = report.id};
    uint16_t collections[HID_CONTACTS] = {0};
    unsigned slot_count = 0;
    int declared_contacts = -1;
    int matched = 0;
    for (int i = 0; i < descriptor->nfields; i++) {
        const struct hid_field *field = &descriptor->f[i];
        if (field->report_id != report.id || !is_touchpad_field(field) || !field->is_input ||
            !(field->flags & HID_MAIN_VARIABLE))
            continue;

        int32_t value = hid_extract_signed(report.body, report.body_bits, field, 0);
        if (field->usage_page == HID_PAGE_BUTTON && field->usage >= 1 && field->usage <= 3) {
            unsigned mask = 1u << (field->usage - 1);
            frame.button_mask |= mask;
            if (value)
                frame.buttons |= mask;
            matched = 1;
            continue;
        }
        if (field->usage_page == HID_PAGE_DIGITIZER && field->usage == HID_USAGE_CONTACT_COUNT) {
            if (value < 0 || value > HID_CONTACTS)
                return -1;
            declared_contacts = value;
            matched = 1;
            frame.has_contacts = 1;
            continue;
        }
        if (!is_contact_field(field))
            continue;

        struct hid_contact *contact =
            find_contact_slot(&frame, collections, &slot_count, field->contact);
        if (!contact || decode_contact_field(field, value, contact) < 0)
            return -1;
        matched = 1;
        frame.has_contacts = 1;
    }
    if (!matched)
        return 0;
    if (finish_touch_frame(&frame, slot_count, declared_contacts) < 0)
        return -1;
    *output = frame;
    return 1;
}

static void update_touch_buttons(struct hid_touch_state *state, const struct hid_touch_frame *frame,
                                 int slot)
{
    if (frame->button_mask) {
        state->button_mask[slot] = frame->button_mask;
        state->button_levels[slot] = frame->buttons;
    }
    state->buttons = 0;
    for (int i = 0; i < HID_MAX_REPORTS; i++)
        state->buttons |= state->button_levels[i];
}

struct touch_anchor {
    unsigned contact_count;
    uint32_t ids[2];
    int x, y;
};

static struct touch_anchor make_touch_anchor(const struct hid_touch_frame *frame)
{
    struct touch_anchor anchor = {0};
    /* Three-finger gestures are not desktop pointer movement. */
    anchor.contact_count = frame->count <= 2 ? frame->count : 0;
    for (unsigned i = 0; i < anchor.contact_count; i++) {
        anchor.x += frame->contacts[i].x;
        anchor.y += frame->contacts[i].y;
        anchor.ids[i] = frame->contacts[i].id;
    }
    if (anchor.contact_count == 2 && anchor.ids[0] > anchor.ids[1]) {
        uint32_t first_id = anchor.ids[0];
        anchor.ids[0] = anchor.ids[1];
        anchor.ids[1] = first_id;
    }
    if (anchor.contact_count) {
        anchor.x /= (int)anchor.contact_count;
        anchor.y /= (int)anchor.contact_count;
    }
    return anchor;
}

static int continues_touch_gesture(const struct hid_touch_state *state,
                                   const struct touch_anchor *anchor, uint8_t report_id)
{
#ifdef USB_HID_NEGCTL_STALE_CONTACT
    (void)report_id;
    /* Negative control: a replacement finger inherits the previous anchor. */
    return anchor->contact_count && state->active;
#else
    return anchor->contact_count && anchor->contact_count == state->active &&
           state->report_id == report_id && anchor->ids[0] == state->ids[0] &&
           anchor->ids[1] == state->ids[1];
#endif
}

static void accumulate_touch_motion(struct hid_touch_state *state,
                                    const struct touch_anchor *anchor,
                                    struct hid_pointer_delta *motion)
{
    if (anchor->contact_count == 1) {
        state->residual_x += anchor->x - state->x;
        state->residual_y += anchor->y - state->y;
        motion->dx = state->residual_x / TOUCH_UNITS_PER_PIXEL;
        motion->dy = state->residual_y / TOUCH_UNITS_PER_PIXEL;
        state->residual_x %= TOUCH_UNITS_PER_PIXEL;
        state->residual_y %= TOUCH_UNITS_PER_PIXEL;
    } else {
        /* Positive finger movement scrolls down in the WM ABI. Accumulate
         * sub-notch motion, so slow scrolling is not lost to truncation. */
        state->residual_wheel += anchor->y - state->y;
        motion->wheel = state->residual_wheel / TOUCH_UNITS_PER_SCROLL_STEP;
        state->residual_wheel %= TOUCH_UNITS_PER_SCROLL_STEP;
    }
}

void hid_touch_apply(struct hid_touch_state *state, const struct hid_touch_frame *frame,
                     int report_slot, struct hid_pointer_delta *motion)
{
    clear_bytes(motion, sizeof *motion);
    if (report_slot < 0 || report_slot >= HID_MAX_REPORTS)
        return;
    update_touch_buttons(state, frame, report_slot);
    motion->buttons = state->buttons;
    if (!frame->has_contacts)
        return;

    /* Button reports may have another ID; contact frames replace the gesture
     * anchor instead of accumulating stale fingers across report IDs. */
    struct touch_anchor anchor = make_touch_anchor(frame);
    if (continues_touch_gesture(state, &anchor, frame->report_id)) {
        accumulate_touch_motion(state, &anchor, motion);
    } else {
        state->residual_x = 0;
        state->residual_y = 0;
        state->residual_wheel = 0;
    }
    state->active = anchor.contact_count;
    state->ids[0] = anchor.ids[0];
    state->ids[1] = anchor.ids[1];
    state->x = anchor.x;
    state->y = anchor.y;
    state->report_id = frame->report_id;
}
