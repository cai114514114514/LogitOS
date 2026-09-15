#include "feature.h"
#include "generic.h"
#include "../usb.h"
#include <stddef.h>

#define HID_REPORT_TYPE_FEATURE 3u
#define HID_INPUT_MODE_TOUCHPAD 3u

/* USB HID 1.11, 7.2: Report ID is included in the data stage when IDs are in
 * use, in addition to wValue. Read before modifying: another field or padding
 * in the same report is not ours to overwrite with an invented zero value. */
static int read_feature(struct usb_device *device, unsigned interface_number,
                         const struct hid_desc *descriptor,
                         const struct hid_field *field, uint8_t *packet)
{
    int bits = hid_report_feature_bits(descriptor, field->report_id);
    unsigned prefix = descriptor->uses_report_ids ? 1u : 0u;
    if (bits <= 0 || bits > HID_MAX_REPORT_BITS)
        return -1;
    unsigned bytes = ((unsigned)bits + 7) / 8 + prefix;
    uint16_t selector = (HID_REPORT_TYPE_FEATURE << 8) | field->report_id;
    int received = usb_control(device, USB_RT_DIR_IN | USB_RT_TYPE_CLASS | USB_RT_RECIP_IF,
                               HID_REQ_GET_REPORT, selector, interface_number, packet, bytes);
    if (received != (int)bytes || (prefix && packet[0] != field->report_id))
        return -1;
    return (int)bytes;
}

static int find_feature(const struct hid_desc *descriptor, uint32_t application,
                         unsigned usage, const struct hid_field **found)
{
    *found = NULL;
    for (int index = 0; index < descriptor->nfields; index++) {
        const struct hid_field *field = &descriptor->f[index];
        if (!field->is_feature || field->application != application ||
            field->usage_page != HID_PAGE_DIGITIZER || field->usage != usage)
            continue;
        if (*found || field->count != 1 || !(field->flags & HID_MAIN_VARIABLE) ||
            (field->flags & (HID_MAIN_CONSTANT | HID_MAIN_RELATIVE)))
            return -1;
        *found = field;
    }
    return 0;
}

/* Switching out of mouse mode is useful only if every advertised contact can
 * be decoded. Hybrid reports need a separate frame assembler; leave those
 * unbound here rather than disabling their mouse collection and losing input. */
static int parallel_contact_capacity(const struct hid_desc *descriptor,
                                      unsigned advertised_maximum)
{
    int found = 0;
    for (int report = 0; report < descriptor->nreports; report++) {
        uint16_t contacts[HID_CONTACTS] = {0};
        unsigned count = 0;
        unsigned declared_maximum = 0;
        for (int index = 0; index < descriptor->nfields; index++) {
            const struct hid_field *field = &descriptor->f[index];
            if (!field->is_input || field->application != HID_APP_TOUCHPAD ||
                field->report_id != descriptor->rep[report].id)
                continue;
            if (field->usage_page == HID_PAGE_DIGITIZER &&
                field->usage == HID_USAGE_CONTACT_COUNT && field->lmax > 0)
                declared_maximum = (unsigned)field->lmax;
            if (!field->contact)
                continue;
            unsigned slot = 0;
            while (slot < count && contacts[slot] != field->contact)
                slot++;
            if (slot == count) {
                if (count == HID_CONTACTS)
                    return -1;
                contacts[count++] = field->contact;
            }
        }
        if (!count)
            continue; /* Separate button reports do not carry contacts. */
        unsigned maximum = advertised_maximum ? advertised_maximum : declared_maximum;
        if (!maximum || maximum > count)
            return -1;
        found = 1;
    }
    return found ? 0 : -1;
}

static void replace_field(uint8_t *body, const struct hid_field *field, uint32_t value)
{
    for (unsigned bit = 0; bit < field->bit_size; bit++) {
        unsigned position = field->bit_offset + bit;
        uint8_t mask = (uint8_t)(1u << (position % 8));
        body[position / 8] &= (uint8_t)~mask;
        if (value & (1u << bit))
            body[position / 8] |= mask;
    }
}

int hid_touchpad_configure(struct usb_device *device, unsigned interface_number,
                           const struct hid_desc *descriptor)
{
    const struct hid_field *mode;
    const struct hid_field *maximum;
    if (find_feature(descriptor, HID_APP_CONFIGURATION, HID_USAGE_INPUT_MODE, &mode))
        return -1;
    if (!mode)
        return 0;
    if (mode->lmin > (int32_t)HID_INPUT_MODE_TOUCHPAD ||
        mode->lmax < (int32_t)HID_INPUT_MODE_TOUCHPAD ||
        mode->bit_size < 2 || mode->bit_size > 32)
        return -1;
    if (find_feature(descriptor, HID_APP_TOUCHPAD, HID_USAGE_CONTACT_MAX, &maximum))
        return -1;

    uint8_t packet[HID_MAX_REPORT_BYTES];
    unsigned prefix = descriptor->uses_report_ids ? 1u : 0u;
    unsigned contact_maximum = 0;
    if (maximum) {
        int bytes = read_feature(device, interface_number, descriptor, maximum, packet);
        if (bytes < 0)
            return -1;
        contact_maximum = hid_extract(packet + prefix, (bytes - prefix) * 8, maximum, 0);
        if (!contact_maximum || contact_maximum > HID_CONTACTS ||
            (int32_t)contact_maximum < maximum->lmin ||
            (int32_t)contact_maximum > maximum->lmax)
            return -1;
    }
    if (parallel_contact_capacity(descriptor, contact_maximum))
        return -1;

    int bytes = read_feature(device, interface_number, descriptor, mode, packet);
    if (bytes < 0)
        return -1;
    replace_field(packet + prefix, mode, HID_INPUT_MODE_TOUCHPAD);
    uint16_t selector = (HID_REPORT_TYPE_FEATURE << 8) | mode->report_id;
#ifndef USB_HID_NEGCTL_SKIP_MODE
    int written = usb_control(device, USB_RT_TYPE_CLASS | USB_RT_RECIP_IF,
                               HID_REQ_SET_REPORT, selector, interface_number, packet, bytes);
    if (written != bytes)
        return -1;
#else
    (void)selector;
#endif
    /* A successful USB status stage alone does not establish that the device
     * selected this mode. Confirm it before input delivery is enabled. */
    bytes = read_feature(device, interface_number, descriptor, mode, packet);
    if (bytes < 0 || hid_extract(packet + prefix, (bytes - prefix) * 8, mode, 0) !=
                     HID_INPUT_MODE_TOUCHPAD)
        return -1;
    return 1;
}
