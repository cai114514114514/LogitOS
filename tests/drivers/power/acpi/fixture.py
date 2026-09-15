#!/usr/bin/env python3
"""Emit a tiny ACPI board for the real interpreter, never a byte-scanning oracle.

AML encodings follow ACPI 6.5 section 20.2. The ASL-equivalent device contents
are deliberately small: battery packages, a counter-backed thermal method,
an AC adapter, and _S5. Hardware-reduced is a property of this test FADT;
the interpreter is compiled with full conventional hardware support.
"""
import pathlib
import struct
import sys


def integer(value):
    if value in (0, 1):
        return bytes([value])
    return b'\x0c' + struct.pack('<I', value)


def string(value):
    return b'\x0d' + value.encode() + b'\x00'


def block(opcode, body):
    # PkgLength includes its own encoded length, but excludes the opcode.
    if len(body) + 1 < 64:
        length = bytes([len(body) + 1])
    else:
        size = len(body) + 2
        assert size < 4096
        length = bytes([0x40 | (size & 15), size >> 4])
    return opcode + length + body


def name(symbol, value):
    return b'\x08' + symbol.encode() + value


def package(values):
    return block(b'\x12', bytes([len(values)]) + b''.join(values))


def method(symbol, statements):
    return block(b'\x14', symbol.encode() + b'\x00' + statements)


def table(signature, payload):
    result = bytearray(struct.pack('<4sIBB6s8sI4sI', signature, 36 + len(payload),
                                   6, 0, b'LOGIT ', b'POWERAML', 1, b'TEST', 1))
    result += payload
    result[9] = (-sum(result)) & 255
    return result


def make_board(scenario):
    battery_state = 15 if scenario == 'empty' else 31
    remaining = 0xffffffff if scenario == 'unknown' else 4200
    status_values = [integer(1), integer(1000), integer(remaining), integer(12000)]
    if scenario == 'bad-battery':
        status_values[2] = string('invalid')
    battery = name('_HID', string('PNP0C0A')) + name('_STA', integer(battery_state))
    if scenario != 'missing-battery-method':
        battery += method('_BST', b'\xa4' + package(status_values))
    info = [0, 5000, 4800, 1, 12000, 400, 200, 1, 1]
    battery += method('_BIF', b'\xa4' + package([integer(v) for v in info] +
                                               [string('model'), string('serial'),
                                                string('LiON'), string('maker')]))
    battery = block(b'\x5b\x82', b'BAT0' + battery)
    adapter = block(b'\x5b\x82', b'AC00' + name('_HID', string('ACPI0003')) +
                    method('_PSR', b'\xa4' + integer(1)))
    # Name(TCNT,0); Method(_TMP) { Increment(TCNT); Return(Add(3000,TCNT)); }
    temperature = b'\x75TCNT\xa4\x72' + integer(3000) + b'TCNT\x00'
    if scenario == 'bad-temperature':
        temperature = b'\xa4' + integer(0xffffffff)
    thermal = block(b'\x5b\x85', b'THRM' + name('TCNT', integer(0)) +
                    method('_TMP', temperature) + method('_CRT', b'\xa4' + integer(3732)))
    sleep = integer(1) if scenario == 'bad-sleep' else package([integer(3), integer(4)])
    aml = name('_S5_', sleep) + block(b'\x10', b'\\_SB_' + battery + adapter) + thermal

    memory = bytearray(16384)
    dsdt = table(b'DSDT', aml)
    memory[0x800:0x800 + len(dsdt)] = dsdt
    fadt = bytearray(276)
    fadt[:36] = table(b'FACP', bytes(240))[:36]
    struct.pack_into('<I', fadt, 40, 0x800)
    struct.pack_into('<I', fadt, 112, 1 << 20)  # HW_REDUCED_ACPI, genuinely no SCI.
    struct.pack_into('<Q', fadt, 140, 0x800)
    fadt[9] = 0
    fadt[9] = (-sum(fadt)) & 255
    memory[0x400:0x400 + len(fadt)] = fadt
    rsdt = table(b'RSDT', struct.pack('<I', 0x400))
    memory[0x200:0x200 + len(rsdt)] = rsdt
    rsdp = bytearray(b'RSD PTR ' + b'\x00' + b'LOGIT ' + b'\x00' + struct.pack('<I', 0x200))
    rsdp[8] = (-sum(rsdp)) & 255
    if scenario == 'bad-checksum':
        # RSDP integrity belongs to the native handoff (separate existing gate).
        # Here exercise the interpreter's fatal-checksum policy on the DSDT.
        memory[0x800 + 9] ^= 1
    memory[0x100:0x114] = rsdp
    return memory


if __name__ == '__main__':
    pathlib.Path(sys.argv[2]).write_bytes(make_board(sys.argv[1]))
