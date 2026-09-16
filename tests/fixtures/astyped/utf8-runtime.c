/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    AtBytes *bytes = at_buffer_new(4);
    /* Generate every Unicode scalar independently from the decoder's state
     * machine. Noncharacters are valid scalar values and must survive too. */
    for (uint32_t scalar = 0; scalar <= 0x10ffff; scalar++) {
        if (scalar >= 0xd800 && scalar <= 0xdfff) {
            continue;
        }
        int width;
        if (scalar < 0x80) {
            width = 1;
        } else if (scalar < 0x800) {
            width = 2;
        } else if (scalar < 0x10000) {
            width = 3;
        } else {
            width = 4;
        }
        uint32_t remaining = scalar;
        for (int index = width - 1; index > 0; index--) {
            bytes->data[index] = 0x80 | (remaining & 0x3f);
            remaining >>= 6;
        }
        static const unsigned char lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
        bytes->data[0] = lead[width] | remaining;
        bytes->length = width;
        AtNativeText text;
        assert(at_bytes_decode(&text, bytes) == 0);
        assert(text.length == width && !memcmp(text.data, bytes->data, (size_t)width));
        for (int cut = 1; cut < width; cut++) {
            bytes->length = cut;
            assert(at_bytes_decode(&text, bytes) == AT_E_CONVERSION);
            assert(text.length == 0);
        }
    }
    at_gc_collect();
    assert(at_gc_live_bytes() == 0);
    return 0;
}
