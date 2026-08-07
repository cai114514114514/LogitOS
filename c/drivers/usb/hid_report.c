#include "hid_report.h"
#include "logit_abi.h"     /* KEY_* for the non-printable keys wm_key() takes */

static void zero(void *p, unsigned long n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* ---------------------------------------------------------------- parser -- */

/* Global item state (HID 1.11 6.2.2.7). Push/Pop save and restore exactly this. */
struct gstate {
    uint16_t usage_page;
    int32_t  lmin, lmax;
    uint8_t  report_size;
    uint8_t  report_id;
    uint16_t report_count;
};

#define HID_MAX_LOCAL_USAGES 64
#define HID_MAX_PUSH 8
#define HID_MAX_REPORT_BITS 4096

struct lstate {
    uint32_t usage[HID_MAX_LOCAL_USAGES];
    int      n_usage;
    uint32_t usage_min, usage_max;
    int      have_range;
};

static int rep_slot(struct hid_desc *o, uint8_t id)
{
    for (int i = 0; i < o->nreports; i++)
        if (o->rep[i].id == id) return i;
    if (o->nreports >= HID_MAX_REPORTS) return -1;
    o->rep[o->nreports].id = id;
    o->rep[o->nreports].in_bits = 0;
    return o->nreports++;
}

/* Unsigned item data. HID item data is little-endian and, for the tags that can
 * be negative (Logical Minimum/Maximum), sign-extended from its own size --
 * which is why a 1-byte Logical Maximum of 0x80 means -128, not 128, and why a
 * mouse whose deltas are (-127..127) needs the signed read below. */
static uint32_t item_u(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_s(const uint8_t *p, int size)
{
    uint32_t v = item_u(p, size);
    if (size == 1 && (v & 0x80))       v |= 0xFFFFFF00u;
    else if (size == 2 && (v & 0x8000)) v |= 0xFFFF0000u;
    return (int32_t)v;
}

int hid_parse_report_desc(const uint8_t *d, int len, struct hid_desc *out)
{
    if (!d || !out || len <= 0) return -1;
    zero(out, sizeof *out);

    struct gstate g, stack[HID_MAX_PUSH];
    struct lstate l;
    int sp = 0;
    zero(&g, sizeof g);
    zero(&l, sizeof l);
    /* Logical Maximum defaults are unspecified; starting at 0/0 means an item
     * that never sets them decodes as unsigned, which is the conservative read. */

    int off = 0;
    while (off < len) {
        uint8_t b = d[off];
        if (b == 0xFE) {
            /* Long item (6.2.2.3): bDataSize at +1, bLongItemTag at +2. Nothing
             * in the wild defines one; skip it, but bounds-check the skip. */
            if (off + 3 > len) return -1;
            int dsize = d[off + 1];
            if (off + 3 + dsize > len) return -1;
            off += 3 + dsize;
            continue;
        }
        int size = b & 0x03;
        if (size == 3) size = 4;                 /* 6.2.2.2: 3 encodes 4 bytes */
        int type = (b >> 2) & 0x03;
        int tag  = (b >> 4) & 0x0F;
        if (off + 1 + size > len) return -1;     /* item runs off the end */
        const uint8_t *dp = d + off + 1;
        off += 1 + size;

        if (type == 1) {                          /* Global */
            switch (tag) {
            case 0x0: g.usage_page   = (uint16_t)item_u(dp, size); break;
            case 0x1: g.lmin         = item_s(dp, size); break;
            case 0x2:
                /* Logical Maximum is nominally signed, but a one-byte 0xFF is
                 * written by half the keyboards on earth to mean 255, not -1 --
                 * a keyboard's key array really does run 0..255. The universal
                 * rule (and what Linux's hid-core does) is: read it signed only
                 * when Logical Minimum was itself negative, which is the only
                 * case where a negative maximum can be meant. */
                g.lmax = (g.lmin < 0) ? item_s(dp, size) : (int32_t)item_u(dp, size);
                break;
            case 0x3: case 0x4: case 0x5: case 0x6: break;   /* physical / unit */
            case 0x7: g.report_size  = (uint8_t)item_u(dp, size); break;
            case 0x8: {
                uint32_t id = item_u(dp, size);
                if (id == 0 || id > 255) return -1;   /* 6.2.2.7: 0 is reserved */
                g.report_id = (uint8_t)id;
                out->uses_report_ids = 1;
                break;
            }
            case 0x9: g.report_count = (uint16_t)item_u(dp, size); break;
            case 0xA:                                    /* Push */
                if (sp >= HID_MAX_PUSH) return -1;
                stack[sp++] = g;
                break;
            case 0xB:                                    /* Pop */
                if (sp <= 0) return -1;
                g = stack[--sp];
                break;
            default: return -1;                          /* reserved global tag */
            }
            continue;
        }

        if (type == 2) {                          /* Local */
            switch (tag) {
            case 0x0: {                           /* Usage */
                uint32_t u = item_u(dp, size);
                /* A 4-byte usage carries its page in the high half (6.2.2.8). */
                if (size == 4) u &= 0xFFFF;
                if (l.n_usage < HID_MAX_LOCAL_USAGES) l.usage[l.n_usage++] = u;
                break;
            }
            case 0x1: l.usage_min = item_u(dp, size) & 0xFFFF; l.have_range |= 1; break;
            case 0x2: l.usage_max = item_u(dp, size) & 0xFFFF; l.have_range |= 2; break;
            default: break;    /* designator/string indices: no effect on layout */
            }
            continue;
        }

        if (type == 0) {                          /* Main */
            if (tag == 0xA) {                     /* Collection */
                zero(&l, sizeof l);
                continue;
            }
            if (tag == 0xC) {                     /* End Collection */
                zero(&l, sizeof l);
                continue;
            }
            int is_input = (tag == 0x8);
            if (!is_input && tag != 0x9 && tag != 0xB) return -1;   /* reserved main */

            uint16_t flags = (uint16_t)item_u(dp, size);
            int count = g.report_count;
            int bits  = g.report_size;
            if (count < 0 || bits <= 0 || bits > 32) {
                /* A zero report size with a nonzero count contributes nothing and
                 * is emitted by real descriptors as a no-op; anything wider than
                 * 32 bits we cannot extract, so refuse rather than mis-decode. */
                if (!(bits == 0 && count == 0)) return -1;
                zero(&l, sizeof l);
                continue;
            }

            int slot = rep_slot(out, g.report_id);
            if (slot < 0) return -1;

            /* Output and Feature items occupy their own reports, not the input
             * one, so only Input advances the input bit cursor. We keep no
             * output layout: setting keyboard LEDs uses SET_REPORT with the boot
             * layout, which every keyboard accepts. */
            uint16_t base = is_input ? out->rep[slot].in_bits : 0;
            long total = (long)count * bits;
            if (is_input) {
                if ((long)base + total > HID_MAX_REPORT_BITS) return -1;
                out->rep[slot].in_bits = (uint16_t)(base + total);
            }

            if (!is_input || (flags & HID_MAIN_CONSTANT)) {
                zero(&l, sizeof l);
                continue;                          /* padding: consumes bits only */
            }

            if (flags & HID_MAIN_VARIABLE) {
                /* One control per report_count, each with its own usage. When
                 * fewer usages than controls are given, the LAST usage repeats
                 * (6.2.2.8) -- that is how "Usage(X) Usage(Y) ... Report Count 3"
                 * descriptors are meant to be read. */
                for (int i = 0; i < count; i++) {
                    if (out->nfields >= HID_MAX_FIELDS) return -1;
                    uint32_t u;
                    if (l.have_range == 3) {
                        if (l.usage_min > l.usage_max) return -1;
                        uint32_t span = l.usage_max - l.usage_min;
                        u = l.usage_min + ((uint32_t)i <= span ? (uint32_t)i : span);
                    } else if (l.n_usage > 0) {
                        u = l.usage[i < l.n_usage ? i : l.n_usage - 1];
                    } else {
                        u = 0;
                    }
                    struct hid_field *f = &out->f[out->nfields++];
                    f->report_id = g.report_id;
                    f->usage_page = g.usage_page;
                    f->usage = u;
                    f->usage_max = u;
                    f->lmin = g.lmin;
                    f->lmax = g.lmax;
                    f->bit_offset = (uint16_t)(base + (long)i * bits);
                    f->bit_size = (uint8_t)bits;
                    f->count = 1;
                    f->flags = flags;
                    f->is_input = 1;
                }
            } else {
                /* Array: `count` slots, each holding an INDEX into the usage
                 * range. This is how a keyboard reports "these six keys are
                 * down" in six bytes rather than 256 bits. */
                if (l.have_range != 3) {
                    /* An array with no Usage Minimum/Maximum is unusable -- there
                     * is nothing for the indices to index. Skip it (it still
                     * consumed its bits above) rather than fail the whole
                     * descriptor, since it decodes to no controls either way. */
                    zero(&l, sizeof l);
                    continue;
                }
                if (l.usage_min > l.usage_max) return -1;
                if (out->nfields >= HID_MAX_FIELDS) return -1;
                struct hid_field *f = &out->f[out->nfields++];
                f->report_id = g.report_id;
                f->usage_page = g.usage_page;
                f->usage = l.usage_min;
                f->usage_max = l.usage_max;
                f->lmin = g.lmin;
                f->lmax = g.lmax;
                f->bit_offset = base;
                f->bit_size = (uint8_t)bits;
                f->count = (uint8_t)(count > 255 ? 255 : count);
                f->flags = flags;
                f->is_input = 1;
            }
            zero(&l, sizeof l);
            continue;
        }
        return -1;                                 /* type 3 is reserved */
    }

    if (sp != 0) return -1;                        /* Push without a matching Pop */
    if (out->nfields == 0) return -1;
    return 0;
}

int hid_report_in_bits(const struct hid_desc *hd, uint8_t report_id)
{
    for (int i = 0; i < hd->nreports; i++)
        if (hd->rep[i].id == report_id) return hd->rep[i].in_bits;
    return -1;
}

/* -------------------------------------------------------------- extract -- */

uint32_t hid_extract(const uint8_t *body, int body_bits, const struct hid_field *f, int index)
{
    if (index < 0 || index >= f->count) return 0;
    int start = f->bit_offset + index * f->bit_size;
    if (start + f->bit_size > body_bits) return 0;   /* short report: no read */

    uint32_t v = 0;
    for (int i = 0; i < f->bit_size; i++) {
        int b = start + i;
        if (body[b >> 3] & (1u << (b & 7)))
            v |= 1u << i;
    }
    return v;
}

int32_t hid_extract_signed(const uint8_t *body, int body_bits, const struct hid_field *f, int index)
{
    uint32_t v = hid_extract(body, body_bits, f, index);
    /* Sign only when the descriptor says the range goes negative. A wheel with
     * Logical Minimum -127 is signed; a button with 0..1 is not, and treating
     * its single bit as a sign bit turns every press into -1. */
    if (f->lmin < 0 && f->bit_size < 32 && (v & (1u << (f->bit_size - 1))))
        v |= ~((1u << f->bit_size) - 1u);
    return (int32_t)v;
}

/* -------------------------------------------------------------- decoders -- */

/* Split a raw report into its ID and body. -> body pointer, or NULL if the
 * report is empty. */
static const uint8_t *split(const struct hid_desc *hd, const uint8_t *rep, int len,
                            uint8_t *id_out, int *body_bits)
{
    if (len <= 0) return 0;
    if (hd->uses_report_ids) {
        *id_out = rep[0];
        *body_bits = (len - 1) * 8;
        return rep + 1;
    }
    *id_out = 0;
    *body_bits = len * 8;
    return rep;
}

int hid_decode_mouse(const struct hid_desc *hd, const uint8_t *rep, int len, struct hid_mouse_state *st)
{
    uint8_t id; int bits;
    const uint8_t *body = split(hd, rep, len, &id, &bits);
    if (!body) return -1;

    int declared = hid_report_in_bits(hd, id);
    if (declared < 0) return 0;                       /* not a report we know */
    if (bits < declared) return -1;                   /* device sent a short report */

    zero(st, sizeof *st);
    int touched = 0;
    for (int i = 0; i < hd->nfields; i++) {
        const struct hid_field *f = &hd->f[i];
        if (f->report_id != id || !f->is_input) continue;
        if (f->usage_page == HID_PAGE_BUTTON) {
            if (f->flags & HID_MAIN_VARIABLE) {
                if (f->usage >= 1 && f->usage <= 32 && hid_extract(body, bits, f, 0))
                    st->buttons |= 1u << (f->usage - 1);
                touched = 1;
            } else {
                /* Array of buttons: each slot names a button by usage. Rare, but
                 * some trackballs do it, and reading it as a variable would
                 * report button 1 held whenever button 3 is pressed. */
                for (int k = 0; k < f->count; k++) {
                    uint32_t u = hid_extract(body, bits, f, k);
                    if (u >= 1 && u <= 32) st->buttons |= 1u << (u - 1);
                }
                touched = 1;
            }
        } else if (f->usage_page == HID_PAGE_DESKTOP && (f->flags & HID_MAIN_VARIABLE)) {
            int32_t v = hid_extract_signed(body, bits, f, 0);
            if (f->usage == HID_USAGE_X)      { st->dx = v; touched = 1; }
            else if (f->usage == HID_USAGE_Y) { st->dy = v; touched = 1; }
            else if (f->usage == HID_USAGE_WHEEL) { st->wheel = v; touched = 1; }
        }
    }
    return touched ? 1 : 0;
}

int hid_decode_keyboard(const struct hid_desc *hd, const uint8_t *rep, int len, struct hid_kbd_state *st)
{
    uint8_t id; int bits;
    const uint8_t *body = split(hd, rep, len, &id, &bits);
    if (!body) return -1;

    int declared = hid_report_in_bits(hd, id);
    if (declared < 0) return 0;
    if (bits < declared) return -1;

    zero(st, sizeof *st);
    int touched = 0;
    for (int i = 0; i < hd->nfields; i++) {
        const struct hid_field *f = &hd->f[i];
        if (f->report_id != id || !f->is_input) continue;
        if (f->usage_page != HID_PAGE_KEYBOARD) continue;

        if (f->flags & HID_MAIN_VARIABLE) {
            /* The eight modifier keys are usages E0..E7 and are reported as
             * individual bits; their bit position in the HID modifier byte is
             * usage - 0xE0, which is exactly the boot-protocol byte 0 layout. */
            if (f->usage >= 0xE0 && f->usage <= 0xE7) {
                if (hid_extract(body, bits, f, 0))
                    st->mods |= (uint8_t)(1u << (f->usage - 0xE0));
                touched = 1;
            }
        } else {
            for (int k = 0; k < f->count && st->nkeys < 8; k++) {
                uint32_t u = hid_extract(body, bits, f, k);
                /* 0 = no key in this slot; 1..3 are the rollover/POST error
                 * codes, which are states, not keys, and must not be typed. */
                if (u == 0 || u > 0xFF) continue;
                if (u <= 3) continue;
                st->keys[st->nkeys++] = (uint8_t)u;
            }
            touched = 1;
        }
    }
    return touched ? 1 : 0;
}

int hid_looks_like_mouse(const struct hid_desc *hd)
{
    int axes = 0, buttons = 0;
    for (int i = 0; i < hd->nfields; i++) {
        const struct hid_field *f = &hd->f[i];
        if (!f->is_input) continue;
        if (f->usage_page == HID_PAGE_DESKTOP &&
            (f->usage == HID_USAGE_X || f->usage == HID_USAGE_Y)) axes++;
        if (f->usage_page == HID_PAGE_BUTTON) buttons++;
    }
    return axes >= 2 && buttons >= 1;
}

int hid_looks_like_keyboard(const struct hid_desc *hd)
{
    int mods = 0, keys = 0;
    for (int i = 0; i < hd->nfields; i++) {
        const struct hid_field *f = &hd->f[i];
        if (!f->is_input || f->usage_page != HID_PAGE_KEYBOARD) continue;
        if (f->flags & HID_MAIN_VARIABLE) mods++;
        else keys += f->count;
    }
    return keys >= 1 && mods >= 1;
}

/* -------------------------------------------------------------- keymap -- */

/* HID Usage Table 1.12 section 10, Keyboard/Keypad page. Indexed by usage;
 * 0 means "we do not type this". The shifted layer is the US layout, matching
 * c/drivers/char/keyboard.c so a USB and a PS/2 keyboard produce identical
 * characters -- an app must not be able to tell which one was typed on. */
static const char kmap[0x68] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x29] = 27, [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x32] = '\\',                                    /* non-US # and ~ */
    [0x33] = ';', [0x34] = '\'', [0x35] = '`',
    [0x36] = ',', [0x37] = '.', [0x38] = '/',
    /* keypad */
    [0x54] = '/', [0x55] = '*', [0x56] = '-', [0x57] = '+', [0x58] = '\n',
    [0x59] = '1', [0x5A] = '2', [0x5B] = '3', [0x5C] = '4', [0x5D] = '5',
    [0x5E] = '6', [0x5F] = '7', [0x60] = '8', [0x61] = '9', [0x62] = '0',
    [0x63] = '.',
};

static const char kmap_shift[0x68] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = 27, [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}', [0x31] = '|',
    [0x32] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~',
    [0x36] = '<', [0x37] = '>', [0x38] = '?',
    [0x54] = '/', [0x55] = '*', [0x56] = '-', [0x57] = '+', [0x58] = '\n',
    [0x63] = '.',
};

int hid_usage_to_key(uint8_t usage, int shift)
{
    switch (usage) {
    case 0x4A: return KEY_HOME;
    case 0x4B: return KEY_PGUP;
    case 0x4D: return KEY_END;
    case 0x4E: return KEY_PGDN;
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x4C: return 0x7F;                 /* Delete */
    default: break;
    }
    if (usage >= sizeof kmap) return 0;
    char c = shift ? kmap_shift[usage] : kmap[usage];
    if (!c && shift) c = kmap[usage];       /* keys with no shifted variant */
    return (unsigned char)c;
}
