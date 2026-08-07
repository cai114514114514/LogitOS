/* Host unit test for c/drivers/usb/usb_desc.c.
 *
 * Descriptors are the first bytes the machine accepts from a device it has
 * never seen. The interesting cases are therefore not the well-formed ones --
 * those are checked here mostly so a later "harden it" change cannot quietly
 * start rejecting real hardware -- but the malformed ones, each of which
 * corresponds to a real way a from-scratch USB stack dies:
 *
 *   bLength 0            the length-driven walk never advances -> infinite loop
 *   bLength past the end  read off the end of the DMA buffer
 *   wTotalLength > buf    parse descriptors the device never sent
 *   endpoint before iface the endpoint is attributed to nothing
 *   9 interfaces          overflow of a fixed table
 *
 * Every one of those must be REJECTED (-1), not partially accepted: a config
 * that half-parsed is a config we would then half-configure.
 *
 * Build (host, no QEMU):
 *   cc -O2 -Wall -Wextra -o build/usb_desc_test tests/unit/usb_desc_test.c \
 *      c/drivers/usb/usb_desc.c -Ic/drivers/usb && ./build/usb_desc_test
 */

#include <stdio.h>
#include <string.h>
#include "usb_desc.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

static void checki(long long got, long long want, const char *what)
{
    if (got != want) { printf("  FAIL %s: got %lld, want %lld\n", what, got, want); failures++; }
}

/* The device descriptor QEMU's usb-kbd reports (full speed, EP0 = 8). */
static const uint8_t kbd_dev[18] = {
    18, USB_DT_DEVICE, 0x00, 0x02,      /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,                    /* class/subclass/proto: per-interface */
    8,                                   /* bMaxPacketSize0 */
    0x27, 0x06, 0x01, 0x00,              /* idVendor 0627 idProduct 0001 */
    0x00, 0x00,                          /* bcdDevice */
    1, 2, 3, 1                           /* iM iP iS bNumConfigurations */
};

/* A boot keyboard configuration: config + interface(HID,boot,keyboard) +
 * HID class descriptor + one interrupt IN endpoint. This is byte-for-byte the
 * shape QEMU's usb-kbd and essentially every real keyboard emit. */
static const uint8_t kbd_cfg[] = {
    /* config, wTotalLength 34 */
    9, USB_DT_CONFIG, 34, 0, 1, 1, 0, 0xA0, 50,
    /* interface 0, alt 0, 1 ep, class 3 sub 1 proto 1 */
    9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUB_BOOT, USB_HID_PROTO_KBD, 0,
    /* HID descriptor: bcdHID 1.11, country 0, 1 class desc, type 0x22 len 65 */
    9, USB_DT_HID, 0x11, 0x01, 0, 1, USB_DT_HID_REPORT, 65, 0,
    /* endpoint 0x81, interrupt, wMaxPacketSize 8, bInterval 10 */
    7, USB_DT_ENDPOINT, 0x81, 0x03, 8, 0, 10
};

/* A composite device: two interfaces, the second with an alternate setting we
 * must skip, and a vendor-specific descriptor we must step over by length. */
static const uint8_t composite_cfg[] = {
    9, USB_DT_CONFIG, 66, 0, 2, 1, 0, 0xA0, 50,
    9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, 0, 0, 0,
    9, USB_DT_HID, 0x11, 0x01, 0, 1, USB_DT_HID_REPORT, 52, 0,
    7, USB_DT_ENDPOINT, 0x81, 0x03, 4, 0, 10,
    9, USB_DT_INTERFACE, 1, 0, 2, USB_CLASS_MASS, 0x06, 0x50, 0,
    7, USB_DT_ENDPOINT, 0x82, 0x02, 0x00, 0x02, 0,     /* bulk IN, 512 */
    7, USB_DT_ENDPOINT, 0x03, 0x02, 0x00, 0x02, 0,     /* bulk OUT, 512 */
    /* alternate setting 1 of interface 1 -- skipped, with its endpoint */
    9, USB_DT_INTERFACE, 1, 1, 1, USB_CLASS_MASS, 0x06, 0x50, 0,
};

int main(void)
{
    struct usb_device_desc dd;
    struct usb_config cfg;

    /* ---------------------------------------------------- device descriptor */
    checki(usb_parse_device_desc(kbd_dev, 18, &dd), 0, "a real device descriptor parses");
    checki(dd.vendor, 0x0627, "idVendor");
    checki(dd.product, 0x0001, "idProduct");
    checki(dd.max_packet0, 8, "bMaxPacketSize0");
    checki(dd.n_configs, 1, "bNumConfigurations");
    checki(dd.bcd_usb, 0x0200, "bcdUSB");

    checki(usb_parse_device_desc(kbd_dev, 17, &dd), -1, "a 17-byte device descriptor is rejected");
    checki(usb_parse_device_desc(kbd_dev, 8, &dd), -1, "the 8-byte first fetch is not a full descriptor");
    {
        uint8_t bad[18]; memcpy(bad, kbd_dev, 18);
        bad[1] = USB_DT_CONFIG;
        checki(usb_parse_device_desc(bad, 18, &dd), -1, "a mistyped device descriptor is rejected");
        memcpy(bad, kbd_dev, 18); bad[0] = 20;
        checki(usb_parse_device_desc(bad, 18, &dd), -1, "bLength != 18 is rejected");
        /* bMaxPacketSize0 has exactly five legal values; anything else misframes
         * every control transfer we would ever schedule. */
        memcpy(bad, kbd_dev, 18); bad[7] = 7;
        checki(usb_parse_device_desc(bad, 18, &dd), -1, "bMaxPacketSize0 = 7 is rejected");
        memcpy(bad, kbd_dev, 18); bad[7] = 0;
        checki(usb_parse_device_desc(bad, 18, &dd), -1, "bMaxPacketSize0 = 0 is rejected");
        memcpy(bad, kbd_dev, 18); bad[7] = 64;
        checki(usb_parse_device_desc(bad, 18, &dd), 0, "bMaxPacketSize0 = 64 (high speed) is accepted");
        memcpy(bad, kbd_dev, 18); bad[7] = 9;
        checki(usb_parse_device_desc(bad, 18, &dd), 0, "bMaxPacketSize0 = 9 (SuperSpeed exponent) is accepted");
        memcpy(bad, kbd_dev, 18); bad[17] = 0;
        checki(usb_parse_device_desc(bad, 18, &dd), -1, "a device with zero configurations is rejected");
    }

    /* ---------------------------------------------------- config: the good */
    checki(usb_config_total_len(kbd_cfg, 9), 34, "wTotalLength read from the first 9 bytes");
    checki(usb_config_total_len(kbd_cfg, 8), 0, "8 bytes is not enough to read wTotalLength");

    checki(usb_parse_config(kbd_cfg, sizeof kbd_cfg, &cfg), 0, "a boot keyboard config parses");
    checki(cfg.n_if, 1, "one interface");
    checki(cfg.value, 1, "bConfigurationValue");
    checki(cfg.iface[0].if_class, USB_CLASS_HID, "interface class HID");
    checki(cfg.iface[0].if_subclass, USB_HID_SUB_BOOT, "boot subclass");
    checki(cfg.iface[0].if_proto, USB_HID_PROTO_KBD, "keyboard protocol");
    checki(cfg.iface[0].has_hid, 1, "the HID class descriptor was seen");
    checki(cfg.iface[0].hid_report_len, 65, "wDescriptorLength of the report descriptor");
    checki(cfg.iface[0].n_ep, 1, "one endpoint");
    checki(cfg.iface[0].ep[0].addr, 0x81, "bEndpointAddress");
    checki(USB_EP_IS_IN(cfg.iface[0].ep[0].addr), 1, "it is an IN endpoint");
    checki(USB_EP_XFER(cfg.iface[0].ep[0].attr), USB_XFER_INT, "it is an interrupt endpoint");
    checki(cfg.iface[0].ep[0].max_packet, 8, "wMaxPacketSize");
    checki(cfg.iface[0].ep[0].interval, 10, "bInterval");

    check(usb_find_ep(&cfg.iface[0], USB_XFER_INT, 1) == &cfg.iface[0].ep[0], "find interrupt IN");
    check(usb_find_ep(&cfg.iface[0], USB_XFER_INT, 0) == NULL, "there is no interrupt OUT");
    check(usb_find_ep(&cfg.iface[0], USB_XFER_BULK, 1) == NULL, "there is no bulk IN");

    /* -------------------------------------------- config: composite + alts */
    checki(usb_parse_config(composite_cfg, sizeof composite_cfg, &cfg), 0, "a composite config parses");
    checki(cfg.n_if, 2, "alternate settings do not create extra interfaces");
    checki(cfg.iface[1].if_class, USB_CLASS_MASS, "second interface is mass storage");
    checki(cfg.iface[1].if_subclass, 0x06, "SCSI transparent");
    checki(cfg.iface[1].if_proto, 0x50, "bulk-only transport");
    checki(cfg.iface[1].n_ep, 2, "two bulk endpoints -- the alt setting's were not merged in");
    checki(cfg.iface[1].ep[0].max_packet, 512, "wMaxPacketSize 512");
    check(usb_find_ep(&cfg.iface[1], USB_XFER_BULK, 1) != NULL, "bulk IN found");
    check(usb_find_ep(&cfg.iface[1], USB_XFER_BULK, 0) != NULL, "bulk OUT found");
    checki(usb_find_ep(&cfg.iface[1], USB_XFER_BULK, 0)->addr, 0x03, "bulk OUT is EP 3");

    /* ------------------------------------------------- config: the hostile */
    {
        uint8_t bad[sizeof kbd_cfg];

        memcpy(bad, kbd_cfg, sizeof bad);
        checki(usb_parse_config(bad, 33, &cfg), -1,
               "a buffer shorter than wTotalLength is rejected (a truncated fetch)");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[9] = 0;         /* interface descriptor claims bLength 0 */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1,
               "bLength 0 is rejected -- it would otherwise never advance the walk");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[9] = 1;
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "bLength 1 cannot hold its own header");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[27] = 40;       /* endpoint descriptor claims to run past the end */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "a descriptor running past wTotalLength is rejected");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[9 + 0] = 8;     /* interface descriptor too short for its own fields */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "a short interface descriptor is rejected");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[27 + 4] = 0; bad[27 + 5] = 0;   /* wMaxPacketSize 0 */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "an endpoint with wMaxPacketSize 0 is rejected");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[27 + 2] = 0x80;                  /* endpoint number 0 */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "endpoint number 0 is rejected (EP0 is never described)");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[1] = USB_DT_DEVICE;
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "a mistyped config descriptor is rejected");

        memcpy(bad, kbd_cfg, sizeof bad);
        bad[2] = 8; bad[3] = 0;              /* wTotalLength 8, shorter than the header */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1, "wTotalLength < 9 is rejected");
    }
    {
        /* An endpoint descriptor before any interface descriptor: nothing to
         * attribute it to. Silently dropping it hides a corrupt blob. */
        uint8_t orphan[] = {
            9, USB_DT_CONFIG, 16, 0, 1, 1, 0, 0xA0, 50,
            7, USB_DT_ENDPOINT, 0x81, 0x03, 8, 0, 10
        };
        checki(usb_parse_config(orphan, sizeof orphan, &cfg), -1, "an endpoint before any interface is rejected");
    }
    {
        /* Nine interfaces against a table of eight. */
        uint8_t many[9 + 9 * 9];
        memset(many, 0, sizeof many);
        many[0] = 9; many[1] = USB_DT_CONFIG;
        many[2] = (uint8_t)(sizeof many); many[3] = 0;
        many[4] = 9; many[5] = 1; many[7] = 0xA0; many[8] = 50;
        for (int i = 0; i < 9; i++) {
            uint8_t *p = many + 9 + i * 9;
            p[0] = 9; p[1] = USB_DT_INTERFACE; p[2] = (uint8_t)i; p[5] = USB_CLASS_HID;
        }
        checki(usb_parse_config(many, sizeof many, &cfg), -1,
               "more interfaces than the fixed table holds is rejected, not truncated");
    }
    {
        /* A HID class descriptor whose per-descriptor triples run past its own
         * bLength -- the read that would walk off a 9-byte descriptor. */
        uint8_t bad[sizeof kbd_cfg];
        memcpy(bad, kbd_cfg, sizeof bad);
        bad[18 + 5] = 4;      /* bNumDescriptors 4, but bLength is still 9 */
        bad[18 + 6] = 0x23;   /* ... and the first triple is not the report one,
                               * so the scan keeps walking off the descriptor */
        checki(usb_parse_config(bad, sizeof bad, &cfg), -1,
               "a HID descriptor claiming more class descriptors than it has room for is rejected");
    }
    {
        /* A config with no interfaces at all cannot be configured. */
        uint8_t empty[] = { 9, USB_DT_CONFIG, 9, 0, 0, 1, 0, 0xA0, 50 };
        checki(usb_parse_config(empty, sizeof empty, &cfg), -1, "a config with no interfaces is rejected");
    }

    /* Null and zero-length inputs must not crash. */
    checki(usb_parse_device_desc(NULL, 18, &dd), -1, "NULL device buffer");
    checki(usb_parse_config(NULL, 34, &cfg), -1, "NULL config buffer");
    checki(usb_parse_config(kbd_cfg, 0, &cfg), -1, "zero-length config buffer");
    checki(usb_config_total_len(NULL, 9), 0, "NULL to usb_config_total_len");

    if (failures) { printf("usb_desc_test: %d FAILURE(S)\n", failures); return 1; }
    printf("usb_desc_test: ok (device + boot-keyboard + composite configs parse; "
           "truncation, bLength 0/1/overrun, orphan endpoints, table overflow and "
           "bad wMaxPacketSize are all rejected)\n");
    return 0;
}
