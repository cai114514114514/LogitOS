"""Memory-profile assertions shared by real guest harnesses.

The former literal ram == '8G' gate silently skipped every high-DMA assertion
when the same scenario was run with '16G'. Sizes select an address constraint,
not a special case named after one test machine.
"""
import re

def ram_bytes(value):
    match = re.fullmatch(r'([1-9][0-9]*)([MG])', value.upper())
    if not match:
        raise ValueError('RAM must be a positive integer followed by M or G')
    return int(match[1]) << (20 if match[2] == 'M' else 30)

def high_ram(value):
    return ram_bytes(value) > (1 << 32)

def require_high_payload(ram, address, completed, minimum):
    if high_ram(ram) and (address < (1 << 32) or completed < minimum):
        raise ValueError('high physical payload completion missing')

def require_capacity(log, ram):
    records = re.findall(rb'\[widecheck\] capacity usable=(\d+) high_free_pages=(\d+)', log)
    if len(records) != 1:
        raise ValueError('missing or ambiguous guest physical capacity record')
    usable, free_high = map(int, records[0])
    expected = ram_bytes(ram)
    # Firmware/runtime/ACPI allocations are reserved. At 16G a 1G allowance
    # still rejects a truncated 8G/4G memory map without assuming identical BIOS
    # and UEFI memory layouts. High payload assertions separately prove access.
    allowance = min(1 << 30, expected // 2)
    if not expected - allowance <= usable <= expected:
        raise ValueError('guest usable capacity does not match requested RAM')
    if high_ram(ram) and free_high < (expected - (2 << 30)) // 4096:
        raise ValueError('guest high-memory free pool is unexpectedly truncated')
    return {'usable_bytes': usable, 'high_free_pages': free_high}
