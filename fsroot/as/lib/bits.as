# bits -- integer bit and flag helpers

def bit(n):
    return 1 << n

def mask(width):
    if width <= 0:
        return 0
    if width >= 64:
        return -1                       # all 64 bits set (i64); 1 << 64 is out of range
    return (1 << width) - 1

def has(flags, bits):
    return (flags & bits) == bits

def set(flags, bits):
    return flags | bits

def clear(flags, bits):
    return flags & ~bits

def toggle(flags, bits):
    return flags ^ bits

def put(flags, bits, on):
    if on:
        return set(flags, bits)
    return clear(flags, bits)

def low_byte(x):
    return x & 0xff

def high_byte(x):
    return (x >> 8) & 0xff

def align_down(x, align):
    if align <= 0:
        raise "align_down() needs a positive alignment"
    return x & ~(align - 1)

def align_up(x, align):
    if align <= 0:
        raise "align_up() needs a positive alignment"
    return (x + align - 1) & ~(align - 1)

def count_ones(x):
    if x < 0:
        raise "count_ones() needs a non-negative integer"
    n = 0
    while x != 0:
        n = n + (x & 1)
        x = x >> 1
    return n

def parity(x):
    return count_ones(x) % 2

def rol(x, shift, width):
    if width <= 0:
        raise "rol() needs a positive width"
    m = mask(width)
    shift = shift % width
    x = x & m
    if shift == 0:                       # avoids x >> width (illegal at width==64)
        return x
    # `>>` is arithmetic, so mask the wrapped-down bits to kill sign extension.
    return ((x << shift) | ((x >> (width - shift)) & mask(shift))) & m

def ror(x, shift, width):
    if width <= 0:
        raise "ror() needs a positive width"
    m = mask(width)
    shift = shift % width
    x = x & m
    if shift == 0:
        return x
    return (((x >> shift) & mask(width - shift)) | (x << (width - shift))) & m

def bytes_le(x, n):
    out = []
    i = 0
    while i < n:
        out.append((x >> (i * 8)) & 0xff)
        i = i + 1
    return out
