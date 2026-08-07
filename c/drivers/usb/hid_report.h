#ifndef LOGIT_HID_REPORT_H
#define LOGIT_HID_REPORT_H

/* HID report descriptors -- the parser, and the two decoders built on it.
 *
 * WHY THIS EXISTS AT ALL, given boot protocol works.
 *   A boot-protocol keyboard sends 8 fixed bytes and a boot mouse 3, and the
 *   HID spec guarantees the layout, so a stack can support QEMU's usb-kbd and
 *   usb-mouse without ever parsing anything. It then fails on a real desk:
 *   boot protocol is optional (bInterfaceSubClass 1), and plenty of shipping
 *   keyboards, every wireless combo receiver worth the name, and essentially
 *   all gaming mice either do not offer it or offer it and put the interesting
 *   controls (extra buttons, the wheel, the 16-bit deltas) only in the report
 *   protocol. "Works on the emulator" and "works on hardware" diverge exactly
 *   here, which is why this is not deferred.
 *
 * WHAT A REPORT DESCRIPTOR IS.
 *   A byte program (HID 1.11 section 6.2.2). Items set GLOBAL state (usage page,
 *   report size, report count, logical range, report ID), then LOCAL state (the
 *   usages this next item covers), then a MAIN item -- Input/Output/Feature --
 *   which consumes that state and emits `report_count` controls of `report_size`
 *   bits each into the report, and clears the local state. Everything is
 *   little-endian and bit-packed with no alignment: a 3-button mouse really does
 *   put three 1-bit buttons and a 5-bit pad in one byte.
 *
 * WHAT THIS PARSER PRODUCES.
 *   A flat list of `struct hid_field`, each with the bit offset and width it
 *   occupies in its report, so decoding a report is bit extraction and a usage
 *   comparison -- no re-walking the descriptor per packet.
 *
 * Pure: no allocation, no controller, no kernel. tests/unit/usb_hid_test.c
 * drives it with the real QEMU usb-kbd and usb-mouse descriptors, a
 * report-ID-using multi-report descriptor, and a pile of malformed ones.
 */

#include <stdint.h>

/* Usage pages (HID Usage Tables 1.12, section 3) */
#define HID_PAGE_DESKTOP  0x01
#define HID_PAGE_KEYBOARD 0x07
#define HID_PAGE_LED      0x08
#define HID_PAGE_BUTTON   0x09
#define HID_PAGE_CONSUMER 0x0C

/* Generic Desktop usages we decode */
#define HID_USAGE_POINTER 0x01
#define HID_USAGE_MOUSE   0x02
#define HID_USAGE_KEYBOARD 0x06
#define HID_USAGE_X       0x30
#define HID_USAGE_Y       0x31
#define HID_USAGE_WHEEL   0x38

/* Main-item data bits (HID 1.11 6.2.2.5) */
#define HID_MAIN_CONSTANT 0x001   /* padding: no usage, ignore */
#define HID_MAIN_VARIABLE 0x002   /* 0 = array (a list of usage indices) */
#define HID_MAIN_RELATIVE 0x004

#define HID_MAX_FIELDS 96
#define HID_MAX_REPORTS 8

struct hid_field {
    uint8_t  report_id;     /* 0 when the descriptor uses no report IDs */
    uint16_t usage_page;
    uint32_t usage;         /* variable: the control's usage.
                             * array:    usage_minimum */
    uint32_t usage_max;     /* array only */
    int32_t  lmin, lmax;
    uint16_t bit_offset;    /* from the start of the report BODY (after the
                             * report-ID byte, when there is one) */
    uint8_t  bit_size;
    uint8_t  count;         /* controls in this field: 1 for a variable control,
                             * report_count for an array */
    uint16_t flags;         /* HID_MAIN_* */
    uint8_t  is_input;
};

struct hid_desc {
    int nfields;
    struct hid_field f[HID_MAX_FIELDS];
    int uses_report_ids;
    int nreports;
    struct { uint8_t id; uint16_t in_bits; } rep[HID_MAX_REPORTS];
};

/* -> 0 on success, -1 on a descriptor that is malformed, unbounded, or larger
 * than our fixed field table. A truncated item, a Pop with no Push, a report
 * that would exceed 4096 bits and a Usage Minimum above its Maximum all fail. */
int hid_parse_report_desc(const uint8_t *d, int len, struct hid_desc *out);

/* Total INPUT bits for `report_id` (0 when the descriptor uses no IDs). */
int hid_report_in_bits(const struct hid_desc *hd, uint8_t report_id);

/* Extract control `index` of `f` from a report body. `body_bits` bounds the
 * read: a field that runs past the end of what the device actually sent yields
 * 0 rather than reading adjacent memory. */
uint32_t hid_extract(const uint8_t *body, int body_bits, const struct hid_field *f, int index);
int32_t  hid_extract_signed(const uint8_t *body, int body_bits, const struct hid_field *f, int index);

/* --- the two decoders --- */

struct hid_mouse_state {
    int dx, dy, wheel;
    uint32_t buttons;       /* bit 0 = button 1 (left), 1 = right, 2 = middle */
};

struct hid_kbd_state {
    uint8_t mods;           /* HID modifier byte layout: bit0 LCtrl .. bit7 RGui */
    uint8_t keys[8];        /* usage codes currently down, 0-padded */
    int nkeys;
};

/* Decode a raw report (INCLUDING the leading report-ID byte when the descriptor
 * uses IDs) into pointer or keyboard state. -> 1 if the report belonged to this
 * decoder and *st was filled, 0 if it did not (wrong report ID, no matching
 * fields), -1 on a report too short for its own descriptor. */
int hid_decode_mouse(const struct hid_desc *hd, const uint8_t *rep, int len, struct hid_mouse_state *st);
int hid_decode_keyboard(const struct hid_desc *hd, const uint8_t *rep, int len, struct hid_kbd_state *st);

/* Does this descriptor describe a pointer / a keyboard at all? Used to pick a
 * decoder when the interface protocol byte says nothing (report-protocol-only
 * devices frequently declare bInterfaceProtocol 0). */
int hid_looks_like_mouse(const struct hid_desc *hd);
int hid_looks_like_keyboard(const struct hid_desc *hd);

/* HID keyboard usage (page 0x07) -> the code wm_key() expects: ASCII for
 * printable keys, KEY_* from logit_abi.h for the arrows and navigation block,
 * 0 for a key we do not map. `shift` selects the shifted layer. */
int hid_usage_to_key(uint8_t usage, int shift);

#endif /* LOGIT_HID_REPORT_H */
