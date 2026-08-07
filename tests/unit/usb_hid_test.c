/* Host unit test for c/drivers/usb/hid_report.c -- the report-descriptor parser
 * and the two decoders built on it.
 *
 * The descriptors below are the REAL ones. The first two are byte for byte what
 * QEMU's usb-kbd and usb-mouse hand out (hw/usb/dev-hid.c), so the boot test in
 * tests/boot/run-usb-test.sh and this test are exercising the same bytes from
 * two directions. The third is a report-ID-multiplexed descriptor of the shape a
 * wireless combo receiver emits -- keyboard on report 1, mouse on report 2 down
 * one endpoint -- which is the case boot protocol cannot express at all and the
 * reason this parser exists.
 *
 * Two traps are asserted explicitly because they silently produce a plausible
 * wrong answer rather than an error:
 *
 *  - Logical Maximum 0xFF in one byte. Read as signed it is -1, and a keyboard
 *    whose key array claims a maximum of -1 decodes to nothing at all.
 *  - Sign extension driven by Logical Minimum, not by field width. A 1-bit
 *    button with range 0..1 read as signed makes every press -1; an 8-bit mouse
 *    delta with range -127..127 read as unsigned makes every leftward move a
 *    jump of +200.
 *
 * Build (host, no QEMU):
 *   cc -O2 -Wall -Wextra -o build/usb_hid_test tests/unit/usb_hid_test.c \
 *      c/drivers/usb/hid_report.c -Ic/drivers/usb -Iinclude/abi && ./build/usb_hid_test
 */

#include <stdio.h>
#include <string.h>
#include "hid_report.h"
#include "logit_abi.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

static void checki(long long got, long long want, const char *what)
{
    if (got != want) { printf("  FAIL %s: got %lld, want %lld\n", what, got, want); failures++; }
}

/* QEMU hw/usb/dev-hid.c: qemu_keyboard_hid_report_descriptor */
static const uint8_t kbd_rd[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xa1, 0x01,        /* Collection (Application) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xe0,        /*   Usage Minimum (224) */
    0x29, 0xe7,        /*   Usage Maximum (231) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute)  -- 8 modifier bits */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x01,        /*   Input (Constant)                  -- the reserved byte */
    0x95, 0x05,        /*   Report Count (5) */
    0x75, 0x01,        /*   Report Size (1) */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x05,        /*   Usage Maximum (5) */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute) -- NOT in the input report */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x03,        /*   Report Size (3) */
    0x91, 0x01,        /*   Output (Constant) */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0xff,        /*   Logical Maximum (255) -- signed, this reads -1 */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0xff,        /*   Usage Maximum (255) */
    0x81, 0x00,        /*   Input (Data, Array)               -- the 6 key slots */
    0xc0               /* End Collection */
};

/* QEMU hw/usb/dev-hid.c: qemu_mouse_hid_report_descriptor */
static const uint8_t mouse_rd[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xa1, 0x01,        /* Collection (Application) */
    0x09, 0x01,        /*   Usage (Pointer) */
    0xa1, 0x00,        /*   Collection (Physical) */
    0x05, 0x09,        /*     Usage Page (Button) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x03,        /*     Usage Maximum (3) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x95, 0x03,        /*     Report Count (3) */
    0x75, 0x01,        /*     Report Size (1) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0x95, 0x01,        /*     Report Count (1) */
    0x75, 0x05,        /*     Report Size (5) */
    0x81, 0x01,        /*     Input (Constant) */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x09, 0x38,        /*     Usage (Wheel) */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7f,        /*     Logical Maximum (127) */
    0x35, 0x00,        /*     Physical Minimum (0) */
    0x45, 0x00,        /*     Physical Maximum (0) */
    0x95, 0x03,        /*     Report Count (3) */
    0x75, 0x08,        /*     Report Size (8) */
    0x81, 0x06,        /*     Input (Data, Variable, Relative) */
    0xc0,              /*   End Collection */
    0xc0               /* End Collection */
};

/* Combo receiver: keyboard as report 1, mouse as report 2, one endpoint. The
 * mouse here has 5 buttons and a 12-bit signed X/Y, which boot protocol cannot
 * describe -- exactly the device class this parser is for. */
static const uint8_t combo_rd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x85, 0x01,                          /*   Report ID (1) */
    0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0xff,
    0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00,
    0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
    0x85, 0x02,                          /*   Report ID (2) */
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x81, 0x02,  /*     5 buttons */
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,  /*     3 bits padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0xf8,                    /*     Logical Minimum (-2048) */
    0x26, 0xff, 0x07,                    /*     Logical Maximum (2047) */
    0x75, 0x0c, 0x95, 0x02, 0x81, 0x06,  /*     two 12-bit relative axes */
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,  /*     8-bit wheel */
    0xc0, 0xc0
};

static const struct hid_field *field_for(const struct hid_desc *hd, uint16_t page, uint32_t usage)
{
    for (int i = 0; i < hd->nfields; i++)
        if (hd->f[i].usage_page == page && hd->f[i].usage == usage) return &hd->f[i];
    return NULL;
}

int main(void)
{
    struct hid_desc hd;

    /* ------------------------------------------------- the boot keyboard -- */
    checki(hid_parse_report_desc(kbd_rd, sizeof kbd_rd, &hd), 0, "QEMU usb-kbd report descriptor parses");
    checki(hd.uses_report_ids, 0, "it uses no report IDs");
    checki(hid_report_in_bits(&hd, 0), 64, "the input report is 64 bits -- the 8-byte boot report");
    check(hid_looks_like_keyboard(&hd), "it looks like a keyboard");
    check(!hid_looks_like_mouse(&hd), "it does not look like a mouse");

    /* The LED Output items must NOT have advanced the input cursor: if they had,
     * the six key slots would sit 8 bits late and every keypress would decode as
     * the wrong key. */
    const struct hid_field *keys = NULL;
    for (int i = 0; i < hd.nfields; i++)
        if (hd.f[i].usage_page == HID_PAGE_KEYBOARD && !(hd.f[i].flags & HID_MAIN_VARIABLE))
            keys = &hd.f[i];
    check(keys != NULL, "the key array field exists");
    if (keys) {
        checki(keys->bit_offset, 16, "the key array starts at bit 16 (after modifiers + reserved)");
        checki(keys->count, 6, "six key slots");
        checki(keys->bit_size, 8, "one byte per slot");
        checki(keys->usage, 0, "usage minimum 0");
        checki(keys->usage_max, 255, "usage maximum 255");
        /* THE TRAP: Logical Maximum 0xFF in one byte. Signed it is -1. */
        checki(keys->lmax, 255, "Logical Maximum 0xFF reads as 255, not -1");
    }
    {
        const struct hid_field *lshift = field_for(&hd, HID_PAGE_KEYBOARD, 0xE1);
        check(lshift != NULL, "the LeftShift modifier bit is a field of its own");
        if (lshift) {
            checki(lshift->bit_offset, 1, "LeftShift is bit 1 of byte 0");
            checki(lshift->bit_size, 1, "one bit");
        }
    }

    /* Decode real boot-keyboard reports. */
    {
        struct hid_kbd_state ks;
        uint8_t rep[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };   /* LeftShift + 'a' */
        checki(hid_decode_keyboard(&hd, rep, 8, &ks), 1, "a keyboard report decodes");
        checki(ks.mods, 0x02, "the LeftShift modifier bit");
        checki(ks.nkeys, 1, "one key down");
        checki(ks.keys[0], 0x04, "usage 0x04 = 'a'");
        checki(hid_usage_to_key(ks.keys[0], ks.mods & 0x22 ? 1 : 0), 'A', "shift makes it 'A'");
        checki(hid_usage_to_key(ks.keys[0], 0), 'a', "unshifted it is 'a'");

        uint8_t six[8] = { 0, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
        checki(hid_decode_keyboard(&hd, six, 8, &ks), 1, "a six-key rollover report decodes");
        checki(ks.nkeys, 6, "all six slots read");
        checki(ks.keys[5], 0x09, "the last slot");

        uint8_t rollover[8] = { 0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
        checki(hid_decode_keyboard(&hd, rollover, 8, &ks), 1, "a rollover-error report decodes");
        checki(ks.nkeys, 0, "usage 0x01 (ErrorRollOver) is a state, not a key, and is not typed");

        uint8_t none[8] = { 0 };
        checki(hid_decode_keyboard(&hd, none, 8, &ks), 1, "an all-keys-up report decodes");
        checki(ks.nkeys, 0, "no keys down");
        checki(ks.mods, 0, "no modifiers");

        /* A device that sends fewer bytes than its own descriptor promises. */
        checki(hid_decode_keyboard(&hd, none, 4, &ks), -1, "a short report is rejected, not padded with zeros");
        checki(hid_decode_keyboard(&hd, none, 0, &ks), -1, "an empty report is rejected");
    }

    /* ---------------------------------------------------- the boot mouse -- */
    checki(hid_parse_report_desc(mouse_rd, sizeof mouse_rd, &hd), 0, "QEMU usb-mouse report descriptor parses");
    checki(hid_report_in_bits(&hd, 0), 32, "the input report is 32 bits -- 4 bytes");
    check(hid_looks_like_mouse(&hd), "it looks like a mouse");
    check(!hid_looks_like_keyboard(&hd), "it does not look like a keyboard");
    {
        const struct hid_field *x = field_for(&hd, HID_PAGE_DESKTOP, HID_USAGE_X);
        const struct hid_field *y = field_for(&hd, HID_PAGE_DESKTOP, HID_USAGE_Y);
        const struct hid_field *w = field_for(&hd, HID_PAGE_DESKTOP, HID_USAGE_WHEEL);
        const struct hid_field *b1 = field_for(&hd, HID_PAGE_BUTTON, 1);
        check(x && y && w && b1, "X, Y, Wheel and Button 1 are all present");
        if (x && y && w && b1) {
            checki(b1->bit_offset, 0, "button 1 is bit 0");
            checki(x->bit_offset, 8, "X starts at bit 8 -- the 3 buttons plus 5 bits of padding");
            checki(y->bit_offset, 16, "Y at bit 16");
            checki(w->bit_offset, 24, "Wheel at bit 24");
            /* THE OTHER TRAP: three usages given, report count three. Each
             * control takes the NEXT usage, not the first one repeated. */
            checki(x->usage, HID_USAGE_X, "the first control got Usage(X)");
            checki(y->usage, HID_USAGE_Y, "the second got Usage(Y), not X again");
            checki(w->usage, HID_USAGE_WHEEL, "the third got Usage(Wheel)");
            checki(x->lmin, -127, "X is a signed field");
            checki(b1->lmin, 0, "a button is not");
        }
    }
    {
        struct hid_mouse_state ms;
        uint8_t rep[4] = { 0x01, 5, 0xFB, 0 };        /* left down, +5, -5 */
        checki(hid_decode_mouse(&hd, rep, 4, &ms), 1, "a mouse report decodes");
        checki(ms.buttons, 1, "left button down");
        checki(ms.dx, 5, "dx +5");
        checki(ms.dy, -5, "dy -5 -- sign extension driven by Logical Minimum");
        checki(ms.wheel, 0, "no wheel");

        uint8_t r2[4] = { 0x06, 0, 0, 0xFF };          /* right+middle, wheel -1 */
        checki(hid_decode_mouse(&hd, r2, 4, &ms), 1, "a second report decodes");
        checki(ms.buttons, 6, "right and middle down");
        checki(ms.wheel, -1, "wheel -1, not 255");

        uint8_t r3[4] = { 0x07, 0x7F, 0x80, 0 };
        checki(hid_decode_mouse(&hd, r3, 4, &ms), 1, "extremes decode");
        checki(ms.dx, 127, "dx at the positive limit");
        checki(ms.dy, -128, "dy at the negative limit");
        checki(ms.buttons, 7, "all three buttons");

        checki(hid_decode_mouse(&hd, r3, 3, &ms), -1, "a short mouse report is rejected");
    }

    /* ------------------------------------- the combo receiver (report IDs) */
    checki(hid_parse_report_desc(combo_rd, sizeof combo_rd, &hd), 0, "a report-ID descriptor parses");
    checki(hd.uses_report_ids, 1, "it uses report IDs");
    checki(hd.nreports, 2, "two reports");
    checki(hid_report_in_bits(&hd, 1), 64, "report 1 (keyboard) is 64 bits");
    checki(hid_report_in_bits(&hd, 2), 8 + 24 + 8, "report 2 (mouse) is 5+3 button bits, two 12-bit axes, 8-bit wheel");
    checki(hid_report_in_bits(&hd, 3), -1, "there is no report 3");
    check(hid_looks_like_keyboard(&hd), "the combo declares a keyboard");
    check(hid_looks_like_mouse(&hd), "the combo declares a mouse too");
    {
        /* Report 1: keyboard. The mouse decoder must decline it, not decode
         * garbage out of it -- both live in the same descriptor. */
        uint8_t k[9] = { 1, 0x01, 0, 0x16, 0, 0, 0, 0, 0 };   /* LeftCtrl + 's' */
        struct hid_kbd_state ks;
        struct hid_mouse_state ms;
        checki(hid_decode_keyboard(&hd, k, 9, &ks), 1, "report 1 decodes as a keyboard");
        checki(ks.mods, 0x01, "LeftCtrl");
        checki(ks.keys[0], 0x16, "usage 0x16 = 's'");
        checki(hid_decode_mouse(&hd, k, 9, &ms), 0, "the mouse decoder declines report 1");

        /* Report 2: mouse. 12-bit axes packed across byte boundaries: buttons
         * 0x11 (1 and 5), X = -1 (0xFFF), Y = +1. */
        uint8_t m[6];
        m[0] = 2;
        m[1] = 0x11;
        m[2] = 0xFF;             /* X bits 0..7  = 0xFF */
        m[3] = 0x1F;             /* X bits 8..11 = 0xF, Y bits 0..3 = 0x1 */
        m[4] = 0x00;             /* Y bits 4..11 = 0 */
        m[5] = 0x02;             /* wheel +2 */
        checki(hid_decode_mouse(&hd, m, 6, &ms), 1, "report 2 decodes as a mouse");
        checki(ms.buttons, 0x11, "buttons 1 and 5 -- a five-button mouse boot protocol cannot express");
        checki(ms.dx, -1, "a 12-bit X of 0xFFF is -1");
        checki(ms.dy, 1, "Y unpacked from across the byte boundary");
        checki(ms.wheel, 2, "wheel +2");
        checki(hid_decode_keyboard(&hd, m, 6, &ks), 0, "the keyboard decoder declines report 2");
    }

    /* --------------------------------------------------- malformed input -- */
    {
        uint8_t truncated[] = { 0x05 };                    /* item with no data */
        checki(hid_parse_report_desc(truncated, sizeof truncated, &hd), -1, "an item truncated at the end is rejected");

        uint8_t truncated4[] = { 0x07, 0x01, 0x02 };       /* 4-byte item, 2 given */
        checki(hid_parse_report_desc(truncated4, sizeof truncated4, &hd), -1, "a 4-byte item with 2 bytes of data is rejected");

        uint8_t pop_no_push[] = { 0xb4, 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xc0 };
        checki(hid_parse_report_desc(pop_no_push, sizeof pop_no_push, &hd), -1, "Pop without Push is rejected");

        uint8_t push_no_pop[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xa4,      /* Push */
            0x75, 0x08, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0xc0
        };
        checki(hid_parse_report_desc(push_no_pop, sizeof push_no_pop, &hd), -1, "Push without Pop is rejected");

        uint8_t huge[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
            0x75, 0x20,                                     /* Report Size 32 */
            0x96, 0xff, 0x7f,                               /* Report Count 32767 */
            0x09, 0x30, 0x81, 0x02, 0xc0
        };
        checki(hid_parse_report_desc(huge, sizeof huge, &hd), -1,
               "a report claiming a megabit is rejected rather than sized into a fixed buffer");

        uint8_t wide[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
            0x75, 0x40,                                     /* Report Size 64 */
            0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0xc0
        };
        checki(hid_parse_report_desc(wide, sizeof wide, &hd), -1, "a 64-bit-wide control is rejected (we extract at most 32)");

        uint8_t bad_id[] = { 0x85, 0x00, 0x75, 0x08, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02 };
        checki(hid_parse_report_desc(bad_id, sizeof bad_id, &hd), -1, "Report ID 0 is reserved and is rejected");

        uint8_t inverted[] = {
            0x05, 0x07, 0x19, 0x20, 0x29, 0x10,             /* Usage Min 32 > Max 16 */
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02
        };
        checki(hid_parse_report_desc(inverted, sizeof inverted, &hd), -1, "Usage Minimum above Usage Maximum is rejected");

        uint8_t empty[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xc0 };
        checki(hid_parse_report_desc(empty, sizeof empty, &hd), -1, "a descriptor with no fields at all is rejected");

        checki(hid_parse_report_desc(NULL, 10, &hd), -1, "NULL descriptor");
        checki(hid_parse_report_desc(kbd_rd, 0, &hd), -1, "zero-length descriptor");
    }

    /* ------------------------------------------------------ the key map -- */
    checki(hid_usage_to_key(0x28, 0), '\n', "Enter");
    checki(hid_usage_to_key(0x2A, 0), '\b', "Backspace");
    checki(hid_usage_to_key(0x2B, 0), '\t', "Tab");
    checki(hid_usage_to_key(0x2C, 0), ' ', "Space");
    checki(hid_usage_to_key(0x29, 0), 27, "Escape");
    checki(hid_usage_to_key(0x1E, 0), '1', "the 1 key");
    checki(hid_usage_to_key(0x1E, 1), '!', "shifted, the 1 key is !");
    checki(hid_usage_to_key(0x27, 0), '0', "the 0 key -- usage 0x27, not 0x1D");
    checki(hid_usage_to_key(0x52, 0), KEY_UP, "arrow up");
    checki(hid_usage_to_key(0x51, 0), KEY_DOWN, "arrow down");
    checki(hid_usage_to_key(0x50, 0), KEY_LEFT, "arrow left");
    checki(hid_usage_to_key(0x4F, 0), KEY_RIGHT, "arrow right");
    checki(hid_usage_to_key(0x4B, 0), KEY_PGUP, "page up");
    checki(hid_usage_to_key(0x4E, 0), KEY_PGDN, "page down");
    checki(hid_usage_to_key(0x4A, 0), KEY_HOME, "home");
    checki(hid_usage_to_key(0x4D, 0), KEY_END, "end");
    checki(hid_usage_to_key(0x00, 0), 0, "usage 0 is not a key");
    checki(hid_usage_to_key(0xE0, 0), 0, "a modifier usage is not a character");
    checki(hid_usage_to_key(0xFF, 0), 0, "an out-of-range usage yields nothing, not a read past the table");
    checki(hid_usage_to_key(0x28, 1), '\n', "a key with no shifted variant keeps its unshifted value");

    /* Every letter, both layers -- this is the table an app's text depends on. */
    for (int i = 0; i < 26; i++) {
        checki(hid_usage_to_key((uint8_t)(0x04 + i), 0), 'a' + i, "lowercase letters");
        checki(hid_usage_to_key((uint8_t)(0x04 + i), 1), 'A' + i, "uppercase letters");
    }

    if (failures) { printf("usb_hid_test: %d FAILURE(S)\n", failures); return 1; }
    printf("usb_hid_test: ok (QEMU usb-kbd and usb-mouse descriptors parse and decode; "
           "report-ID multiplexing; 12-bit cross-byte axes; Logical Maximum 0xFF; "
           "sign extension by Logical Minimum; truncated/Push-Pop/oversized/inverted "
           "descriptors rejected; full US keymap)\n");
    return 0;
}
