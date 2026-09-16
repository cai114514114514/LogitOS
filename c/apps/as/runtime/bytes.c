/* SPDX-License-Identifier: MIT */
#include "native.h"

int at_bytes_decode(AtNativeText *out, const AtBytes *bytes)
{
    *out = (AtNativeText){"", 0};
    int64_t offset = 0;
    while (offset < bytes->length) {
        unsigned char first = bytes->data[offset++];
        if (first < 0x80) {
            continue;
        }
        int trailing;
        uint32_t point;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            trailing = 1;
            point = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            trailing = 2;
            point = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            trailing = 3;
            point = first & 0x07;
            minimum = 0x10000;
        } else {
            return AT_E_CONVERSION;
        }
        if (bytes->length - offset < trailing) {
            return AT_E_CONVERSION;
        }
        for (int index = 0; index < trailing; index++) {
            unsigned char next = bytes->data[offset++];
            if ((next & 0xc0) != 0x80) {
                return AT_E_CONVERSION;
            }
            point = (point << 6) | (next & 0x3f);
        }
        /* Continuation-byte shape alone accepts overlong encodings, UTF-16
         * surrogates and values beyond Unicode. Reject those scalar values;
         * NUL, BOM and noncharacters remain valid and are preserved exactly. */
        if (point < minimum || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) {
            return AT_E_CONVERSION;
        }
    }
    if (bytes->length) {
        /* Both owners are immutable, so decoding needs no second allocation.
         * The generated str scanner marks this interior pointer and retains
         * its Bytes owner. Empty views use static storage, never a past-end root. */
        *out = (AtNativeText){(const char *)bytes->data, bytes->length};
    }
    return 0;
}
