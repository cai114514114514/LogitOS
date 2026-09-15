#!/usr/bin/env python3
"""Full ACPI board whose real AML battery/thermal methods access EC Fields."""
import pathlib
import struct
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from fixture import block, integer, name, package, string, table


def method(symbol, statements, arguments=0):
    return block(b'\x14', symbol.encode() + bytes([arguments]) + statements)


def field_length(bits):
    if bits < 64:
        return bytes([bits])
    return bytes([0x40 | (bits & 15), bits >> 4])


def board(scenario):
    data_port, control_port = 0x260, 0x264
    if scenario == 'overlap':
        control_port = data_port
    decode = 0 if scenario == 'decode10' else 1
    crs = b''
    for port in (data_port, control_port):
        if scenario == 'fixed-io':
            crs += b'\x4b' + struct.pack('<HB', port, 1)
        else:
            crs += b'\x47' + struct.pack('<BHHBB', decode, port, port, 1, 1)
    crs += b'\x79\x00'
    region = b'\x5b\x80ECG0\x03' + integer(0x10) + integer(0x22)
    fields = b'ECG0\x01CFG0\x08INI0\x08EVNT\x08'
    fields += b'\x00' + field_length(13 * 8) + b'REMN\x10'
    fields += b'\x00' + field_length(14 * 8) + b'TMPV\x10'
    fields = block(b'\x5b\x81', fields)
    # Store(Arg1,RDY0); Store(0xa5,CFG0). _INI copies RDY0 to an EC field.
    reg = method('_REG', b'\x75RGCN\x70\x69RDY0\x70' + integer(0xa5) + b'CFG0', 2)
    ini = method('_INI', b'\x70RDY0INI0')
    query = method('_Q42', b'\x75QCNT\x70' + integer(0x5a) + b'EVNT')
    ec = name('_HID', string('PNP0C09')) + name('_STA', integer(15))
    ec += name('_UID', integer(7)) + name('_GLK', integer(1 if scenario == 'shared' else 0))
    gpe = package([integer(0), integer(3)]) if scenario == 'gpe-package' else integer(3)
    ec += name('_GPE', gpe) + name('_CRS', block(b'\x11', integer(len(crs)) + crs))
    ec += name('RDY0', integer(0)) + name('RGCN', integer(0)) + name('QCNT', integer(0)) + region + fields + reg + ini + query

    # Store(Package(...),Local0); Store(REMN,Index(Local0,2)); Return(Local0).
    # The Field is evaluated before publication, not returned as a name string.
    bst = b'\x70' + package([integer(1), integer(1000), integer(0), integer(12000)]) + b'\x60'
    bst += b'\x70REMN\x88\x60' + integer(2) + b'\x00\xa4\x60'
    info = [0, 5000, 4800, 1, 12000, 400, 200, 1, 1]
    battery = name('_HID', string('PNP0C0A')) + name('_STA', integer(31))
    battery += method('_BST', bst)
    battery += method('_BIF', b'\xa4' + package([integer(v) for v in info] + [string('x')] * 4))
    ec += block(b'\x5b\x82', b'BAT0' + battery)
    ec += block(b'\x5b\x85', b'THRM' + method('_TMP', b'\xa4TMPV'))
    ec = block(b'\x5b\x82', b'EC00' + ec)
    if scenario == 'gpe-owned':
        gpe_handler = block(b'\x10', b'\\_GPE' + method('_E03', b'\xa4\x00'))
    else:
        gpe_handler = b''
    aml = name('_S5_', package([integer(3), integer(4)]))
    aml += block(b'\x10', b'\\_SB_' + ec) + gpe_handler

    memory = bytearray(16384)
    dsdt = table(b'DSDT', aml)
    memory[0x800:0x800 + len(dsdt)] = dsdt
    fadt = bytearray(table(b'FACP', bytes(240)))
    struct.pack_into('<II', fadt, 36, 0xf00, 0x800)
    struct.pack_into('<H', fadt, 46, 9)
    struct.pack_into('<I', fadt, 56, 0x4000)
    struct.pack_into('<I', fadt, 64, 0x4004)
    struct.pack_into('<I', fadt, 76, 0x4008)
    struct.pack_into('<I', fadt, 80, 0x4010)
    fadt[88], fadt[89], fadt[91], fadt[92] = 4, 2, 4, 2
    struct.pack_into('<QQ', fadt, 132, 0xf00, 0x800)
    fadt[9] = 0
    fadt[9] = (-sum(fadt)) & 255
    memory[0x400:0x400 + len(fadt)] = fadt
    facs = bytearray(64)
    struct.pack_into('<4sI', facs, 0, b'FACS', 64)
    facs[32] = 2
    memory[0xf00:0xf40] = facs
    entries = [0x400]
    if scenario.startswith('ecdt'):
        gas = lambda port: struct.pack('<BBBBQ', 1, 8, 0, 1, port)
        ecdt_control = data_port if scenario == 'ecdt-mismatch' else control_port
        payload = gas(ecdt_control) + gas(data_port) + struct.pack('<IB', 7, 3)
        payload += b'\\_SB_.EC00\x00'
        if scenario == 'ecdt-unterminated':
            payload = payload[:-1] + b'X'
        ecdt = table(b'ECDT', payload)
        memory[0x600:0x600 + len(ecdt)] = ecdt
        entries.append(0x600)
    rsdt = table(b'RSDT', b''.join(struct.pack('<I', entry) for entry in entries))
    memory[0x200:0x200 + len(rsdt)] = rsdt
    rsdp = bytearray(b'RSD PTR ' + b'\x00LOGIT \x00' + struct.pack('<I', 0x200))
    rsdp[8] = (-sum(rsdp)) & 255
    memory[0x100:0x114] = rsdp
    return memory


if __name__ == '__main__':
    pathlib.Path(sys.argv[2]).write_bytes(board(sys.argv[1]))
