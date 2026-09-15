#!/usr/bin/env python3
"""Independent file/TOC byte oracle for the complete normal-PF package.

Expected MEC code and JT sizes follow Linux v6.12 amdgpu_cgs.c consumers,
not a round-trip through the production parser. Each output padding byte is
checked too. This is offline evidence; it proves neither mapping nor execution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def word(b, offset):
    return struct.unpack_from('<I', b, offset)[0]


def check(image, firmware, base):
    blobs = {name: (firmware / ('polaris10_' + name + '.bin')).read_bytes()
             for name in ('sdma', 'sdma1', 'ce', 'pfp', 'me', 'mec', 'rlc')}
    # Both VI jump tables intentionally select MEC1 even if MEC2 is present.
    kinds = [('rlc', 10), ('ce', 3), ('pfp', 4), ('me', 5), ('mec', 6),
             ('mec', 7), ('mec', 8), ('sdma', 1), ('sdma1', 2)]
    expected = bytearray(4096)
    struct.pack_into('<II', expected, 0, 1, 9)
    entries = []
    for i, (kind, image_id) in enumerate(kinds):
        b = blobs[kind]
        payload = b[word(b, 24):word(b, 24) + word(b, 20)]
        toc_size = len(payload)
        if image_id == 6:
            toc_size = word(b, 36) * 4
        if image_id in (7, 8):
            payload = payload[word(b, 36) * 4:(word(b, 36) + word(b, 40)) * 4]
            toc_size = len(payload)
        offset = len(expected)
        address = base + offset
        flags = int(image_id in (6, 10))
        struct.pack_into('<HHIIIIIHH', expected, 8 + i * 28,
                         image_id, word(b, 16), address >> 32, address & 0xffffffff,
                         0, 0, toc_size, flags, 0)
        expected.extend(payload)
        expected.extend(bytes((-len(payload)) % 4096))
        actual = image[offset:offset + len(payload)]
        if actual != payload:
            raise AssertionError(f'payload mismatch for image {image_id}')
        entries.append(dict(id=image_id, offset=offset, toc_bytes=toc_size,
                            allocation_bytes=(len(payload) + 4095) & ~4095))
    assert image == expected, 'TOC, padding, or total image size mismatch'
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tool', type=Path, required=True)
    ap.add_argument('--negative-tool', type=Path, required=True)
    ap.add_argument('--firmware-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    base = 0x123400000
    records = {}
    for name, tool in [('negative', a.negative_tool), ('positive', a.tool)]:
        output = a.out / (name + '.bin')
        run = subprocess.run([str(tool.resolve()), str(a.firmware_dir.resolve()), str(output), hex(base)],
                             capture_output=True, text=True)
        (a.out / (name + '.log')).write_text(run.stdout + run.stderr)
        assert run.returncode == 0, run.stdout + run.stderr
        blob = output.read_bytes()
        try:
            entries = check(blob, a.firmware_dir, base)
        except AssertionError as error:
            if name != 'negative' or str(error) != 'payload mismatch for image 8':
                raise
            records[name] = {'expected_failure': str(error)}
            print('BUNDLE_FILES_NEGCTL: corrupted JT2 rejected')
        else:
            assert name == 'positive', 'negative-control package unexpectedly passed'
            records[name] = dict(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(), entries=entries)
    records.update(required_mask='0x47e', present_mask='0x5fe', missing_mask='0',
                   mapped=False, loaded=False, physical_hardware_tested=False)
    records['inputs'] = {p.name: dict(bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                         for p in sorted(a.firmware_dir.glob('polaris10_*.bin'))}
    (a.out / 'result.json').write_text(json.dumps(records, indent=2) + '\n')
    print('BUNDLE_FILES: all 9 TOC entries, all payloads and padding match; mapped=0 loaded=0')


if __name__ == '__main__':
    main()
