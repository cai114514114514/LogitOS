#include "usb_desc.h"

static void zero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

int usb_parse_device_desc(const uint8_t *buf, int len, struct usb_device_desc *out)
{
    if (!buf || !out || len < 18) return -1;
    if (buf[0] != 18 || buf[1] != USB_DT_DEVICE) return -1;

    zero(out, sizeof *out);
    out->bcd_usb       = le16(buf + 2);
    out->dev_class     = buf[4];
    out->dev_subclass  = buf[5];
    out->dev_proto     = buf[6];
    out->max_packet0   = buf[7];
    out->vendor        = le16(buf + 8);
    out->product       = le16(buf + 10);
    out->bcd_device    = le16(buf + 12);
    out->i_manufacturer = buf[14];
    out->i_product     = buf[15];
    out->i_serial      = buf[16];
    out->n_configs     = buf[17];

    /* bMaxPacketSize0 is legal only as 8/16/32/64 (USB 2.0 9.6.1) or, for
     * SuperSpeed, the exponent 9. Anything else and every control transfer we
     * ever schedule is misframed, so refuse the device instead of guessing 8. */
    switch (out->max_packet0) {
    case 8: case 16: case 32: case 64: case 9: break;
    default: return -1;
    }
    if (out->n_configs == 0) return -1;
    return 0;
}

uint16_t usb_config_total_len(const uint8_t *buf, int len)
{
    if (!buf || len < 9) return 0;
    if (buf[0] != 9 || buf[1] != USB_DT_CONFIG) return 0;
    return le16(buf + 2);
}

int usb_parse_config(const uint8_t *buf, int len, struct usb_config *out)
{
    if (!buf || !out || len < 9) return -1;
    if (buf[0] != 9 || buf[1] != USB_DT_CONFIG) return -1;

    uint16_t total = le16(buf + 2);
    /* The device told us how long this configuration is. If we are holding less
     * than that, we are holding a truncated one -- a short control transfer, a
     * device that lied, or a buffer we sized wrong. Any of the three makes the
     * interface list below incomplete in a way nothing downstream can detect. */
    if (total < 9 || total > len) return -1;

    zero(out, sizeof *out);
    out->total_len  = total;
    out->value      = buf[5];
    out->attributes = buf[7];
    out->max_power  = buf[8];

    int cur = -1;              /* index in out->iface of the interface being filled */
    int skipping_alt = 0;      /* inside an alternate setting: drop its endpoints */
    int n_if_declared = buf[4];

    int off = 9;
    while (off + 2 <= (int)total) {
        int blen = buf[off];
        int btype = buf[off + 1];

        /* bLength 0 is the malformed case that matters: it does not advance the
         * cursor, so a length-driven walk spins forever. bLength 1 cannot even
         * hold its own header. Both are rejected outright rather than skipped,
         * because a descriptor blob containing one is not trustworthy. */
        if (blen < 2) return -1;
        if (off + blen > (int)total) return -1;   /* runs past what we were given */

        switch (btype) {
        case USB_DT_INTERFACE: {
            if (blen < 9) return -1;
            uint8_t alt = buf[off + 3];
            if (alt != 0) { skipping_alt = 1; break; }
            skipping_alt = 0;
            if (out->n_if >= USB_MAX_IF) return -1;
            cur = out->n_if++;
            struct usb_interface *it = &out->iface[cur];
            it->num          = buf[off + 2];
            it->alt          = alt;
            it->n_ep         = 0;
            it->if_class     = buf[off + 5];
            it->if_subclass  = buf[off + 6];
            it->if_proto     = buf[off + 7];
            it->has_hid      = 0;
            it->hid_report_len = 0;
            break;
        }
        case USB_DT_ENDPOINT: {
            if (blen < 7) return -1;
            if (skipping_alt) break;
            /* An endpoint descriptor before any interface descriptor is
             * structurally impossible (9.6.6) and would otherwise be silently
             * dropped, hiding a corrupt blob. */
            if (cur < 0) return -1;
            struct usb_interface *it = &out->iface[cur];
            if (it->n_ep >= USB_MAX_EP_PER_IF) return -1;
            struct usb_endpoint *ep = &it->ep[it->n_ep++];
            ep->addr       = buf[off + 2];
            ep->attr       = buf[off + 3];
            ep->max_packet = le16(buf + off + 4) & 0x7FF;   /* bits 10:0; 12:11 = mult */
            ep->interval   = buf[off + 6];
            if (ep->max_packet == 0) return -1;
            if (USB_EP_NUM(ep->addr) == 0) return -1;       /* EP0 is never described */
            break;
        }
        case USB_DT_HID: {
            /* HID 1.11 6.2.1: bLength 9 + 3 bytes per extra class descriptor.
             * The first (bDescriptorType, wDescriptorLength) pair is at +6. */
            if (blen < 9) return -1;
            if (skipping_alt || cur < 0) break;
            struct usb_interface *it = &out->iface[cur];
            it->has_hid = 1;
            uint8_t n_desc = buf[off + 5];
            for (int i = 0; i < n_desc; i++) {
                int p = off + 6 + i * 3;
                if (p + 3 > off + blen) return -1;
                if (buf[p] == USB_DT_HID_REPORT) {
                    it->hid_report_len = le16(buf + p + 1);
                    break;
                }
            }
            break;
        }
        default:
            break;    /* vendor / IAD / SuperSpeed companion: skipped by length */
        }
        off += blen;
    }

    if (out->n_if == 0) return -1;
    /* bNumInterfaces is a claim we can check against what we actually saw. A
     * mismatch means the walk lost a descriptor. */
    if (n_if_declared != out->n_if && n_if_declared > USB_MAX_IF) return -1;
    return 0;
}

const struct usb_endpoint *usb_find_ep(const struct usb_interface *iface, int xfer, int in)
{
    if (!iface) return 0;
    for (int i = 0; i < iface->n_ep; i++) {
        const struct usb_endpoint *ep = &iface->ep[i];
        if (USB_EP_XFER(ep->attr) != xfer) continue;
        if (!USB_EP_IS_IN(ep->addr) != !in) continue;
        return ep;
    }
    return 0;
}
