#!/usr/bin/env python3
"""Measure the load-time cost of raising the logical-CPU ceiling 8 -> 32."""
import argparse
import json
from pathlib import Path
import struct


def sections(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise ValueError(f"{path}: expected ELF64 little-endian image")
    section_offset = struct.unpack_from("<Q", data, 0x28)[0]
    section_size = struct.unpack_from("<H", data, 0x3A)[0]
    section_count = struct.unpack_from("<H", data, 0x3C)[0]
    names_index = struct.unpack_from("<H", data, 0x3E)[0]
    headers = [
        struct.unpack_from("<IIQQQQIIQQ", data,
                           section_offset + i * section_size)
        for i in range(section_count)
    ]
    names_header = headers[names_index]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    result = {}
    for header in headers:
        end = names.find(b"\0", header[0])
        name = names[header[0]:end].decode(errors="replace")
        result[name] = header[5]
    result["file"] = path.stat().st_size
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("production", type=Path)
    parser.add_argument("cap8", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    production = sections(args.production)
    cap8 = sections(args.cap8)
    keys = ("file", ".text", ".rodata", ".data", ".bss")
    report = {
        "production": {key: production.get(key, 0) for key in keys},
        "cap8": {key: cap8.get(key, 0) for key in keys},
        "delta": {key: production.get(key, 0) - cap8.get(key, 0)
                  for key in keys},
    }
    delta = report["delta"][".bss"]
    # 24 additional 16-KiB syscall stacks alone cost 393,216 bytes.  Keep an
    # upper guard so an accidental per-CPU multi-megabyte table cannot hide in
    # an otherwise successful high-core boot.
    if delta < 24 * 16 * 1024 or delta > 1024 * 1024:
        raise SystemExit(f"unexpected 32-slot BSS delta: {delta} bytes")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"xeon_e5_size: bss +{delta} bytes; "
          f"ELF file {report['delta']['file']:+d} bytes")


if __name__ == "__main__":
    main()
