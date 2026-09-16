/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "system.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    at_caps_init();
    AtNativeText text;
    const char input[] = "abc";
    int64_t value = 999;
    assert(at_system_call(&value, 0, 0, 0, 0) == AT_E_RUNTIME && value == 0);
    assert(at_memory_text(&text, input, sizeof input, 3, 0) == 0);
    assert(text.length == 3 && !memcmp(text.data, input, 3));
    /* The physical allocation is larger than the declared bound: this must
     * fail because of the checked bound, not an incidental ASan page fault. */
    assert(at_memory_text(&text, input, 2, 3, 0) == AT_E_INDEX);
    assert(text.data == NULL && text.length == 0);
    assert(at_memory_text(&text, input, 3, 4, 1) == AT_E_VALUE);
    assert(at_memory_text(&text, input, 4, 4, 1) == 0 && text.length == 3);
    assert(at_memory_text(&text, "\xff", 1, 1, 0) == AT_E_CONVERSION);

    /* Revocation occurs after earlier successful calls and live allocations.
     * Both entry points must consult current authority before memory/kernel IO. */
    at_caps_set(0, NULL);
    assert(at_memory_text(&text, input, sizeof input, 3, 0) == AT_E_PERMISSION);
    value = 999;
    assert(at_system_call(&value, 0, 0, 0, 0) == AT_E_PERMISSION && value == 0);
    at_gc_collect();
    assert(at_gc_live_bytes() == 0);
    return 0;
}
