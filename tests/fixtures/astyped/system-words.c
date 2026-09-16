/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>

int64_t probe_system_call(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    switch (number) {
    case 9:
        assert(a == 0 && b == 0 && c == 0);
        return 73;
    case 11:
        assert(a == (uint64_t)-7 && b == 0 && c == 0);
        return -123;
    case 12:
        assert(a == 0 && b == UINT64_MAX && c == 0);
        return 17;
    default:
        assert(number == 1 && a == 2 && b == 3 && c == 4);
        return 99;
    }
}
