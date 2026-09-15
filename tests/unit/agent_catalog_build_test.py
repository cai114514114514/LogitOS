#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify built owned AEX images; build/link evidence, never guest execution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from agent_catalog import entries, manifest


def require(ok, message):
    if not ok:
        raise ValueError(message)


def elf_symbols(data):
    require(data[:7] == b'\x7fELF\x02\x01\x01', 'ELF64 little-endian image required')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    require(header[2] == 62 and header[1] in (2, 3), 'x86-64 executable required')
    entry, phoff, shoff = header[4:7]
    phentsize, phnum, shentsize, shnum = header[9:13]
    require(phentsize == 56 and shentsize == 64, 'unexpected ELF table shape')
    require(phoff + phnum * phentsize <= len(data) and
            shoff + shnum * shentsize <= len(data), 'ELF table exceeds image')
    executable = []
    for i in range(phnum):
        ph = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if ph[0] == 1 and ph[1] & 1:
            require(ph[2] + ph[5] <= len(data), 'executable segment exceeds image')
            executable.append((ph[3], ph[3] + ph[5]))
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
                for i in range(shnum)]
    symbols = {}
    for sh in sections:
        if sh[1] != 2:
            continue
        require(sh[9] == 24 and sh[6] < shnum, 'invalid symbol table')
        strings = sections[sh[6]]
        require(sh[4] + sh[5] <= len(data) and strings[4] + strings[5] <= len(data),
                'symbol data exceeds image')
        names = data[strings[4]:strings[4] + strings[5]]
        for off in range(sh[4], sh[4] + sh[5], 24):
            name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, off)
            if not index:
                continue
            require(name < len(names) and b'\0' in names[name:], 'invalid symbol name')
            name = names[name:names.index(b'\0', name)].decode('utf-8')
            symbols[name] = value
    for name in ('_agent_start', '_start', 'ag_activation', 'ag_worker', 'ag_self', 'ag_gui_dispatch'):
        require(name in symbols, 'missing linked symbol ' + name)
        require(any(lo <= symbols[name] < hi for lo, hi in executable),
                'symbol has no executable file bytes: ' + name)
    require(entry == symbols['_agent_start'], 'ELF entry bypasses activation')
    require(entry != symbols['_start'], 'legacy CRT replaced activation entry')
    return entry, symbols


def verify(app, build, disassembler):
    path = build / (app['name'] + '.aex')
    require(path.is_file(), 'AEX target was not produced')
    data = path.read_bytes()
    require(len(data) >= 64 and data[:4] == b'AEX1', 'invalid AEX header')
    version, flags = struct.unpack_from('<HH', data, 4)
    require(version == 3, 'AEX version is ' + str(version) + ', expected 3')
    require(flags & 3 == (1 if app['kind'] == 'gui' else 2), 'wrong GUI/CLI classification')
    header_size = struct.unpack_from('<H', data, 52)[0]
    elf_size = struct.unpack_from('<I', data, 60)[0]
    require(64 <= header_size <= 16384 and header_size % 4096 == 0 and
            header_size + elf_size == len(data), 'invalid AEX image boundaries')
    records, pos = {}, 64
    while pos < header_size:
        require(pos + 8 <= header_size, 'truncated TLV header')
        kind, size = struct.unpack_from('<II', data, pos)
        end = pos + 8 + size
        require(end <= header_size, 'TLV exceeds metadata')
        records.setdefault(kind, []).append(data[pos + 8:end])
        pos = (end + 7) & ~7
    require(pos == header_size, 'TLV alignment exceeds metadata')
    for tag in (0x44495841, 0x544e4741, 0x43524341):
        require(len(records.get(tag, [])) == 1, 'missing/duplicate required v3 metadata')
    require(records[0x44495841][0] == app['id'].encode() + b'\0', 'AppID differs from catalog')
    expected = manifest(app)
    require(records[0x544e4741][0] == struct.pack('<8I', *expected.values()),
            'manifest differs from catalog')
    elf = data[header_size:]
    require(records[0x43524341][0] == struct.pack('<I', zlib.crc32(elf) & 0xffffffff),
            'AEX executable CRC mismatch')
    elf_path = path.with_suffix('.elf')
    require(elf_path.is_file() and elf_path.read_bytes() == elf, 'AEX does not contain linked ELF')
    digest = hashlib.sha256(elf).hexdigest()
    side = json.loads(elf_path.with_suffix('.elf.agent.json').read_text())
    require(side == dict(id=app['id'], manifest=expected, elf_sha256=digest),
            'activation sidecar differs from actual ELF/catalog')
    entry, symbols = elf_symbols(elf)
    result = subprocess.run([disassembler, '-d', '--disassemble-symbols=_agent_start', str(elf_path)],
                            capture_output=True, text=True, timeout=15)
    require(result.returncode == 0, 'disassembler rejected ELF: ' + result.stderr)
    require(re.search(r'\bcall\w*\s+0x[0-9a-f]+ <ag_activation>', result.stdout),
            'entry does not call linked activation function')
    require(re.search(r'\bjmp\w*\s+0x[0-9a-f]+ <_start>', result.stdout),
            'entry does not preserve original CRT handoff')
    assembly = build / 'entry-disassembly' / (app['name'] + '.txt')
    assembly.parent.mkdir(parents=True, exist_ok=True)
    assembly.write_text(result.stdout)
    return dict(version=version, entry=hex(entry), elf_bytes=elf_size, aex_bytes=len(data),
                elf_sha256=digest, aex_sha256=hashlib.sha256(data).hexdigest(),
                activation=hex(symbols['ag_activation']), legacy_entry=hex(symbols['_start']),
                entry_disassembly=str(assembly))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--report-name', default='coverage')
    parser.add_argument('--objdump', default=shutil.which('llvm-objdump') or
                        '/opt/homebrew/opt/llvm/bin/llvm-objdump')
    args = parser.parse_args()
    build = args.build.resolve()
    rows = []
    for app in entries():
        row = {key: app[key] for key in ('name', 'id', 'path', 'source', 'kind')}
        try:
            row.update(verify(app, build, args.objdump))
            row['status'] = 'PASS'
        except (ValueError, OSError, KeyError, struct.error, subprocess.SubprocessError) as error:
            row.update(status='FAIL', reason=str(error))
            print('FAIL: ' + app['name'] + ': ' + str(error))
        rows.append(row)
    passed = sum(row['status'] == 'PASS' for row in rows)
    report = dict(boundary='Built artifact and ELF entry verification; applications were not executed.',
                  build=str(build), passed=passed, total=len(rows), apps=rows)
    (build / (args.report_name + '.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    lines = [f'Owned AEX artifact coverage: {passed}/{len(rows)}', '', report['boundary'], '',
             '| Application | Stable AppID | AEX | ELF entry | Result |',
             '|---|---|---|---|---|']
    for row in rows:
        lines.append('| ' + ' | '.join([row['name'], row['id'], str(row.get('version', '-')),
                                       row.get('entry', '-'), row.get('reason', row['status'])]) + ' |')
    (build / (args.report_name + '.md')).write_text('\n'.join(lines) + '\n')
    print(f'AGENT_CATALOG_ARTIFACTS passed={passed} total={len(rows)}')
    return int(passed != len(rows))


if __name__ == '__main__':
    raise SystemExit(main())
