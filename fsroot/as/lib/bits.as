# aether: 3.0
# Signed i64 bit-pattern helpers. Arithmetic still checks overflow; only the
# operations whose purpose is to move/discard bits explicitly request wrapping.

def bit(n: i64) -> i64:
    return wrapping_shl(1, n)

def mask(width: i64) -> i64:
    if width <= 0:
        return 0
    if width >= 64:
        return -1
    # Both steps wrap: mask(63) crosses the sign bit before subtracting one.
    return wrapping_sub(wrapping_shl(1, width), 1)

def has(flags: i64, bits: i64) -> bool:
    return (flags & bits) == bits

def set(flags: i64, bits: i64) -> i64:
    return flags | bits

def clear(flags: i64, bits: i64) -> i64:
    return flags & ~bits

def toggle(flags: i64, bits: i64) -> i64:
    return flags ^ bits

def put(flags: i64, bits: i64, on: bool) -> i64:
    return set(flags, bits) if on else clear(flags, bits)

def low_byte(x: i64) -> i64:
    return x & 0xff

def high_byte(x: i64) -> i64:
    return (x >> 8) & 0xff

def align_down(x: i64, align: i64) -> i64:
    if align <= 0:
        raise ValueError("align_down() needs a positive alignment")
    return x & ~(align - 1)

def align_up(x: i64, align: i64) -> i64:
    if align <= 0:
        raise ValueError("align_up() needs a positive alignment")
    # Keep the established bit-mask API (callers supply power-of-two alignment).
    # The addition is checked: an unrepresentable aligned address is an error.
    return (x + (align - 1)) & ~(align - 1)

def count_ones(x: i64) -> i64:
    if x < 0:
        raise ValueError("count_ones() needs a non-negative integer")
    count = 0
    while x != 0:
        # Remove one set bit per iteration; x is positive, so subtraction fits.
        x = x & (x - 1)
        count += 1
    return count

def parity(x: i64) -> i64:
    return count_ones(x) % 2

def _rotation(shift: i64, width: i64) -> i64:
    if width <= 0 or width > 64:
        raise ValueError("rotation needs a width from 1 to 64")
    shift = shift % width
    return shift + width if shift < 0 else shift

def rol(x: i64, shift: i64, width: i64) -> i64:
    shift = _rotation(shift, width)
    x = x & mask(width)
    if shift == 0:
        return x
    # Arithmetic right shift fills with sign bits; the mask keeps only the
    # wrapped-down bits. The zero case avoids an invalid shift by 64.
    lower = (x >> (width - shift)) & mask(shift)
    return (wrapping_shl(x, shift) | lower) & mask(width)

def ror(x: i64, shift: i64, width: i64) -> i64:
    shift = _rotation(shift, width)
    x = x & mask(width)
    if shift == 0:
        return x
    lower = (x >> shift) & mask(width - shift)
    return (lower | wrapping_shl(x, width - shift)) & mask(width)

def bytes_le(x: i64, n: i64) -> List[i64]:
    out: List[i64] = []
    # Like A2, asking for a ninth byte raises the invalid-shift ValueError;
    # negative/zero counts return an empty list. No host endianness is assumed.
    for i in range(n):
        out.append((x >> (i * 8)) & 0xff)
    return out
