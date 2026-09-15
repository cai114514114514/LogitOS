#!/usr/bin/env python3
"""Boot and compare the serial Multiboot2 dumps from loader and GRUB."""

import argparse
import itertools
import json
import os
import subprocess
import sys
import tempfile
import time


CONSUMED = {6, 8, 14, 15}


def fail(message):
    print(f"FAIL: {message}")
    raise SystemExit(1)


def boot_command(qemu, iso, serial="stdio", disk=None):
    # `-vga none` verifies tag 8 stays optional; virtio-gpu is the independent
    # shipping framebuffer path a full kernel boot needs after MB2 validation.
    command = [
        qemu, "-machine", "q35", "-m", "512M", "-smp", "1",
        "-display", "none", "-serial", serial, "-monitor", "none",
        "-no-reboot", "-vga", "none",
        "-device", "virtio-gpu-pci",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-boot", "d", "-cdrom", iso,
    ]
    if disk:
        # Snapshot mode keeps three destructive controls from sharing guest
        # filesystem writes while still exercising the ordinary root disk.
        command += ["-drive", f"file={disk},format=raw,if=virtio,snapshot=on"]
    return command


def boot(qemu, iso, timeout=20, disk=None):
    command = boot_command(qemu, iso, disk=disk)
    try:
        run = subprocess.run(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        return output.replace("\r", ""), None
    return run.stdout.replace("\r", ""), run.returncode


def qmp_execute(reader, writer, command):
    writer.write(json.dumps(command) + "\n")
    writer.flush()
    while True:
        line = reader.readline()
        if not line:
            fail("QMP disconnected before replying")
        reply = json.loads(line)
        if "error" in reply:
            fail(f"QMP command failed: {reply['error']}")
        if "return" in reply:
            return reply["return"]


def boot_and_read_vga(qemu, iso, disk=None):
    """Boot until handoff, then read the kernel's VGA error cells via QMP."""
    # Keep QEMU's transient serial and pmemsave artifacts outside the build;
    # stdio QMP is deliberate because this sandbox forbids listener sockets.
    with tempfile.TemporaryDirectory(prefix="logit-bios-", dir="/tmp") as temp:
        serial_path = os.path.join(temp, "serial.log")
        vga_path = os.path.join(temp, "vga.bin")
        command = boot_command(qemu, iso, f"file:{serial_path}", disk=disk)
        command += ["-qmp", "stdio"]
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True)
        deadline = time.monotonic() + 20
        try:
            greeting = json.loads(process.stdout.readline())
            if "QMP" not in greeting:
                fail("QMP greeting was malformed")
            qmp_execute(process.stdout, process.stdin,
                        {"execute": "qmp_capabilities"})
            output = ""
            while time.monotonic() < deadline:
                if os.path.exists(serial_path):
                    with open(serial_path, encoding="utf-8", errors="replace") as serial:
                        output = serial.read().replace("\r", "")
                if "LOADER ENTER KERNEL" in output:
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.02)
            if "LOADER ENTER KERNEL" not in output:
                return output, ""
            # check_multiboot writes six character/attribute pairs before
            # halting.  Reading guest physical VGA memory observes the
            # kernel's own error path even though it predates serial init.
            time.sleep(0.05)
            command_line = f'pmemsave 0xb8000 12 "{vga_path}"'
            response = qmp_execute(
                process.stdout, process.stdin,
                {"execute": "human-monitor-command",
                 "arguments": {"command-line": command_line}})
            if not os.path.exists(vga_path):
                fail(f"QMP pmemsave produced no VGA snapshot: {response}")
            with open(vga_path, "rb") as vga:
                cells = vga.read(12)
            text = cells[0::2].decode("ascii", "replace")
            return output, text
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


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


def print_guest_evidence(output):
    """Print only stable serial evidence, not QEMU's host-side banner noise."""
    for line in output.splitlines():
        if (line.startswith(("LOGIT", "LOADER", "MB2", "ERR:")) or
                "LOGIT_BOOT_OK" in line):
            print(line)


def compare_consumed(left, right, left_name="OURS", right_name="GRUB"):
    left_consumed = consumed_blocks(left)
    right_consumed = consumed_blocks(right)
    if left_consumed != right_consumed:
        print("CONSUMED-TAG DIFFERENCE (adjacent lines):")
        print_side_by_side([line for block in left_consumed for line in block],
                           [line for block in right_consumed for line in block],
                           left_name, right_name)
        fail("consumed Multiboot2 tags 6/8/14/15 differ")


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
    print_side_by_side(ours, grub)
    compare_consumed(ours, grub)
    print("PASS: bios-mb2 differential -- consumed tags 6/8/14/15 are equivalent")


def kernel_compare(args):
    ours_output, _ = boot(args.qemu, args.ours, disk=args.disk)
    grub_output, _ = boot(args.qemu, args.grub, disk=args.disk)
    ours = dump_lines(ours_output)
    grub = dump_lines(grub_output)
    validate(ours)
    validate(grub)
    compare_consumed(ours, grub)
    print("PASS: bios-boot differential -- consumed tags 6/8/14/15 are equivalent")


def kernel_check(args):
    # Full initialization includes guest-timed wait and clock selftests after
    # the desktop appears; the MB2 dump/control paths finish within 20 seconds,
    # but the product end marker needs the same wider budget as boot gates.
    output, _ = boot(args.qemu, args.iso, timeout=60, disk=args.disk)
    if "LOADER ENTER KERNEL" not in output:
        fail("our loader did not reach the kernel handoff\n" + output)
    if "LOGIT_BOOT_OK" not in output:
        fail("our loader entered the kernel but it did not reach LOGIT_BOOT_OK\n" + output)
    # Quote the kernel's marker as its own line so the make transcript is an
    # artifact-bound boot result rather than a summary invented by the host.
    print("LOGIT_BOOT_OK")
    print("PASS: bios-boot -- our loader reached the kernel end-of-init marker")


def kernel_control(args):
    if args.reason == "bad-magic":
        output, vga_text = boot_and_read_vga(args.qemu, args.iso, disk=args.disk)
    else:
        output, _ = boot(args.qemu, args.iso, disk=args.disk)
        vga_text = ""
    print_guest_evidence(output)
    reached_boot = "LOGIT_BOOT_OK" in output
    if args.reason == "bad-magic":
        if "LOADER ENTER KERNEL" not in output:
            fail("bad-magic control did not reach the kernel handoff")
        print(vga_text)
        if vga_text != "ERR: 0" or reached_boot:
            fail("bad-magic control did not print ERR: 0 and stop before LOGIT_BOOT_OK")
        print("PASS: bad-magic control was watched failing: ERR: 0; LOGIT_BOOT_OK absent")
        return
    if args.reason == "short-segment":
        if ("LOADER CONTROL PT_LOAD ONE PAGE SHORT" not in output or
                "LOADER ENTER KERNEL" not in output):
            fail("short-segment control did not omit one page and attempt entry")
        if reached_boot:
            fail("short-segment control unexpectedly reached LOGIT_BOOT_OK")
        print("PASS: short-segment control was watched failing: LOGIT_BOOT_OK absent")
        return
    if args.reason == "skip-bss-zero":
        if ("LOADER CONTROL BSS ZERO SKIPPED" not in output or
                "LOADER ENTER KERNEL" not in output):
            fail("BSS control did not skip a non-empty tail and attempt entry")
        if reached_boot:
            print("SKIP: BSS failure half -- the kernel still reached LOGIT_BOOT_OK when the loader "
                  "left the tail unzeroed; this QEMU run did not expose non-zero initial RAM")
        else:
            print("PASS: BSS-zero control was watched failing: LOGIT_BOOT_OK absent")
        return


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
    parser.add_argument("--disk")
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
    kernel = sub.add_parser("kernel-compare")
    kernel.add_argument("ours")
    kernel.add_argument("grub")
    kernel.set_defaults(func=kernel_compare)
    kernel_check_parser = sub.add_parser("kernel-check")
    kernel_check_parser.add_argument("iso")
    kernel_check_parser.set_defaults(func=kernel_check)
    control = sub.add_parser("kernel-control")
    control.add_argument("iso")
    control.add_argument("--reason", required=True,
                         choices=("bad-magic", "short-segment", "skip-bss-zero"))
    control.set_defaults(func=kernel_control)
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
