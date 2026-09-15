#!/usr/bin/env python3
"""Boot and compare the serial Multiboot2 dumps from loader and GRUB."""

import argparse
import itertools
import subprocess
import sys


CONSUMED = {6, 8, 14, 15}


def fail(message):
    print(f"FAIL: {message}")
    raise SystemExit(1)


def boot(qemu, iso):
    command = [
        qemu, "-machine", "q35", "-m", "512M", "-smp", "1",
        "-display", "none", "-serial", "stdio", "-monitor", "none",
        "-no-reboot", "-vga", "none",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "d", "-cdrom", iso,
    ]
    try:
        run = subprocess.run(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=20)
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        return output.replace("\r", ""), None
    return run.stdout.replace("\r", ""), run.returncode


def dump_lines(output, require_pass=True):
    lines = output.splitlines()
    try:
        start = lines.index("MB2 BEGIN")
    except ValueError:
        if require_pass:
            fail("guest did not print MB2 BEGIN\n" + output)
        return []
    result = None
    for end in range(start, len(lines)):
        if lines[end].startswith("MB2 RESULT "):
            result = lines[start:end + 1]
            break
    if result is None:
        if require_pass:
            fail("guest dump did not reach a result\n" + output)
        return lines[start:]
    if require_pass and result[-1] != "MB2 RESULT PASS":
        fail("guest rejected its Multiboot2 block\n" + "\n".join(result))
    return result


def validate(lines, require_no_framebuffer=True):
    if len(lines) < 5 or not lines[1].startswith("MB2 TOTAL "):
        fail("dump is missing its total_size line")
    try:
        total = int(lines[1].split()[2], 16)
    except (IndexError, ValueError):
        fail("dump total_size is not hexadecimal")
    tags = []
    mmap = []
    for line in lines:
        fields = line.split()
        if fields[:2] == ["MB2", "TAG"]:
            if len(fields) != 4:
                fail(f"unstable tag line: {line}")
            tags.append((int(fields[2], 16), int(fields[3], 16)))
        elif fields[:2] == ["MB2", "MMAP"]:
            if len(fields) != 5:
                fail(f"unstable memory-map line: {line}")
            mmap.append(tuple(int(value, 16) for value in fields[2:]))
    if not tags or tags[-1] != (0, 8):
        fail("type-0 size-8 end tag is not last")
    if sum((size + 7) & ~7 for _, size in tags) + 8 != total:
        fail("total_size does not match header plus aligned tag bytes")
    if not any(tag_type == 6 for tag_type, _ in tags):
        fail("tag 6 is absent")
    if not any(kind == 1 and length != 0 for _, length, kind in mmap):
        fail("tag 6 has no non-empty usable region")
    if require_no_framebuffer and any(tag_type == 8 for tag_type, _ in tags):
        fail("tag 8 is present under the default -vga none path")
    return tags, mmap


def consumed_blocks(lines):
    blocks = []
    current = None
    for line in lines:
        fields = line.split()
        if fields[:2] == ["MB2", "TAG"]:
            tag_type = int(fields[2], 16)
            current = [line] if tag_type in CONSUMED else None
            if current is not None:
                blocks.append(current)
        elif current is not None and fields[:2] in (["MB2", "MMAP"],
                                                    ["MB2", "ACPI"],
                                                    ["MB2", "FB"]):
            current.append(line)
    return tuple(tuple(block) for block in blocks)


def print_side_by_side(left, right, left_name="OURS", right_name="GRUB"):
    width = max([len(left_name)] + [len(line) for line in left])
    print(f"{left_name:<{width}} | {right_name}")
    for a, b in itertools.zip_longest(left, right, fillvalue=""):
        print(f"{a:<{width}} | {b}")


def check_one(args):
    output, _ = boot(args.qemu, args.iso)
    lines = dump_lines(output)
    tags, mmap = validate(lines)
    print("\n".join(lines))
    print(f"PASS: bios-mb2 parsed total_size with {len(tags)} tags and "
          f"{len(mmap)} memory-map entries; tag 8 absent under -vga none")


def a20_control(args):
    output, _ = boot(args.qemu, args.iso)
    if "LOADER CONTROL A20 PRE_ENABLED" in output:
        lines = dump_lines(output)
        validate(lines)
        print("SKIP: A20 skipped-enable failure half -- SeaBIOS entered with A20 already enabled; "
              "the alias verification still ran and passed")
        return
    if "LOADER A20 VERIFY FAIL" in output and "MB2 BEGIN" not in output:
        print("PASS: A20 skipped-enable control was watched failing at the alias verification")
        return
    fail("A20 skipped-enable control neither failed verification nor proved firmware pre-enabled A20\n"
         + output)


def compare(args):
    ours_output, _ = boot(args.qemu, args.ours)
    grub_output, _ = boot(args.qemu, args.grub)
    ours = dump_lines(ours_output)
    grub = dump_lines(grub_output)
    validate(ours)
    validate(grub)
    ours_consumed = consumed_blocks(ours)
    grub_consumed = consumed_blocks(grub)
    print_side_by_side(ours, grub)
    if ours_consumed != grub_consumed:
        print("CONSUMED-TAG DIFFERENCE (adjacent lines):")
        print_side_by_side([line for block in ours_consumed for line in block],
                           [line for block in grub_consumed for line in block])
        fail("consumed Multiboot2 tags 6/8/14/15 differ")
    print("PASS: bios-mb2 differential -- consumed tags 6/8/14/15 are equivalent")


def expect_difference(args):
    control_output, _ = boot(args.qemu, args.control)
    grub_output, _ = boot(args.qemu, args.grub)
    control = dump_lines(control_output)
    grub = dump_lines(grub_output)
    validate(control)
    validate(grub)
    left = consumed_blocks(control)
    right = consumed_blocks(grub)
    print_side_by_side([line for block in left for line in block],
                       [line for block in right for line in block],
                       "CONTROL", "GRUB")
    if left == right:
        fail(f"{args.reason} control did not make the differential fail")
    if args.reason == "truncated-e820":
        control_count = sum(line.startswith("MB2 MMAP ") for line in control)
        grub_count = sum(line.startswith("MB2 MMAP ") for line in grub)
        if control_count >= grub_count:
            fail("truncated-E820 dump did not show fewer regions")
        print(f"PASS: truncated-e820 control was watched failing: "
              f"control {control_count} regions, GRUB {grub_count} regions")
    elif args.reason == "bad-rsdp-checksum":
        if any(line.startswith("MB2 ACPI ") for line in control):
            fail("bad-RSDP-checksum control emitted an ACPI tag")
        if not any(line.startswith("MB2 ACPI ") for line in grub):
            fail("GRUB provided no ACPI tag, so the checksum control is unobservable")
        print("PASS: bad-rsdp-checksum control was watched failing: control emitted no ACPI tag")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    sub = parser.add_subparsers(dest="action", required=True)
    one = sub.add_parser("check")
    one.add_argument("iso")
    one.set_defaults(func=check_one)
    a20 = sub.add_parser("a20-control")
    a20.add_argument("iso")
    a20.set_defaults(func=a20_control)
    diff = sub.add_parser("compare")
    diff.add_argument("ours")
    diff.add_argument("grub")
    diff.set_defaults(func=compare)
    neg = sub.add_parser("expect-difference")
    neg.add_argument("control")
    neg.add_argument("grub")
    neg.add_argument("--reason", required=True,
                     choices=("truncated-e820", "bad-rsdp-checksum"))
    neg.set_defaults(func=expect_difference)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
