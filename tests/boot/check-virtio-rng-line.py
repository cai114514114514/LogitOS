#!/usr/bin/env python3
"""Check the boot RNG diagnostic without accepting leaked entropy bytes."""

import argparse
import re


VERDICT = re.compile(
    r"^VIRTIO_RNG_SELFTEST \S+ n_a=(\d+) n_b=(\d+) "
    r"zero_a=([01]) zero_b=([01]) same=([01])$"
)
RAW_BYTES = re.compile(r"(?:^|\s)[ab]=[0-9a-fA-F]+(?:\s|$)")


def check(line):
    if RAW_BYTES.search(line):
        raise ValueError("raw entropy bytes appeared in the serial log")
    match = VERDICT.fullmatch(line)
    if not match:
        raise ValueError("missing or malformed RNG verdict")
    n_a, n_b, zero_a, zero_b, same = map(int, match.groups())
    if (n_a, n_b) != (16, 16):
        raise ValueError("both device reads must contain 16 bytes")
    if zero_a or zero_b or same:
        raise ValueError("device reads were zero or identical")


def control():
    # This was the old serial shape. It must fail for the disclosure itself,
    # not merely because the new verdict grammar differs.
    old = ("VIRTIO_RNG_SELFTEST virtio-rng a=" + "01" * 16 +
           " b=" + "02" * 16 + " zero_a=0 zero_b=0 same=0")
    try:
        check(old)
    except ValueError as error:
        if str(error) != "raw entropy bytes appeared in the serial log":
            raise
        print("NEGCTL RED: old boot diagnostic exposed 32 entropy bytes")
    else:
        raise AssertionError("raw entropy control was accepted")
    check("VIRTIO_RNG_SELFTEST virtio-rng n_a=16 n_b=16 zero_a=0 zero_b=0 same=0")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("line", nargs="?")
    parser.add_argument("--control", action="store_true")
    args = parser.parse_args()
    try:
        if args.control:
            control()
        elif args.line is not None:
            check(args.line)
            print("PASS: two independent nonzero device reads; no raw entropy in serial")
        else:
            parser.error("a serial line or --control is required")
    except (ValueError, AssertionError) as error:
        raise SystemExit(f"FAIL: {error}") from error
