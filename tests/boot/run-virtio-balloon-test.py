#!/usr/bin/env python3
"""virtio-balloon boot test: does the driver give real physical frames back to
the host when asked, and give them back to itself when asked to stop?

c/drivers/virtio/virtio_balloon.c has no syscall path into it (same shape as
virtio_rng.c before it, and for the same reason: no caller has asked for one
yet), so this drives it the same way run-virtio-rng-test.sh drives virtio-rng
-- off the driver's own boot-serial self-test lines -- plus one thing the RNG
test could not do: virtio-balloon has a HOST-SIDE observable. QMP's
`query-balloon` reads QEMU's own device model, not anything the guest prints,
so a passing run has TWO INDEPENDENT witnesses that inflation actually
happened: the guest's own kprintf (computed from pmm_free_frames(), inside the
kernel under test) and the host's view of the wire protocol (computed by a
completely separate implementation, QEMU's hw/virtio/virtio-balloon.c, from
watching config.actual go by). Either one lying alone would not make the
other agree.

Two things are checked, matching virtio_balloon.c's own two proof shapes:

  1. VIRTIO_BALLOON_SELFTEST -- a small (4-frame) inflate/deflate round trip
     the driver runs on itself at probe() regardless of anything the host
     does. free_after_inflate must be LOWER than free_before by exactly the
     frame count inflated (not "close to" -- a stub that moved nothing would
     still boot and still print a line), and free_after_deflate must recover
     to free_before exactly. This is the same shape as the RNG self-test's
     "neither read is all-zero, and the two reads differ": a control that
     could not fail structurally would prove nothing.

  2. VIRTIO_BALLOON_TARGET -- the real path. `balloon N` is sent over QMP the
     instant the socket answers, which is BEFORE the guest has executed a
     single instruction (QEMU's device model is realised, and QMP already
     live, before the vCPU runs) -- so by the time this driver's probe() runs
     during the guest's own PCI enumeration, config.num_pages already holds
     the host's target, no runtime re-poll required (see virtio_balloon.h for
     why this driver does not re-poll after boot). The test asserts: the
     guest's own accounting (free_before - free_after, in frames) equals the
     target requested (converted to frames), AND QMP's `query-balloon`
     independently reports a logical VM size consistent with what the guest
     wrote to config.actual.

QMP's `balloon` command and `query-balloon`'s `actual` both speak in LOGICAL
VM SIZE (vm_ram_size - balloon_size), per QEMU's own qapi/machine.json doc
comment for `balloon` ("value: the target logical size of the VM in bytes...
balloon_size = vm_ram_size - @value") -- the opposite convention from the
virtio wire protocol, where config.num_pages/actual are the BALLOON's own
page count (virtio-v1.1 sec 5.5.4, and what this driver's kprintf lines
report). hw/virtio/virtio-balloon.c's virtio_balloon_set_config() is the
bridge: it is the callback QEMU runs whenever the guest writes its config
space, and it does `dev->actual = le32_to_cpu(config.actual)`, and
virtio_balloon_stat() computes query-balloon's `actual` as
`get_current_ram_size() - (dev->actual << VIRTIO_BALLOON_PFN_SHIFT)` -- so
query-balloon's number is derived from the SAME bytes this driver wrote to
CFG_ACTUAL, converted through the RAM-size subtraction. This script does that
arithmetic explicitly rather than comparing page counts to byte totals as if
they were the same unit.

ENABLE=0 is the negative control: the identical kernel and command line with
`-device virtio-balloon-pci` removed. Both positive assertions (self-test
line present; query-balloon showing a real device) must be UNSATISFIABLE
there -- run with ENABLE=0 and confirm it fails; that is the control, not a
separate passing test.

    python3 tests/boot/run-virtio-balloon-test.py <iso> <disk.img> [--enable=0]
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
RAM_MB = int(os.environ.get("BALLOON_RAM_MB", "512"))
# Target left for the guest after ballooning: RAM_MB - TARGET_TAKE_MB.
TARGET_TAKE_MB = int(os.environ.get("BALLOON_TAKE_MB", "64"))


def qmp_connect(sock_path, timeout=120.0):
    s = socket.socket(socket.AF_UNIX)
    deadline = time.time() + timeout
    while True:
        try:
            s.connect(sock_path)
            break
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.05)
    f = s.makefile("rw")
    f.readline()  # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    f.readline()
    return f


def qmp_cmd(f, execute, arguments=None):
    d = {"execute": execute}
    if arguments is not None:
        d["arguments"] = arguments
    f.write(json.dumps(d) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            return None
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m


def read(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def main():
    if len(sys.argv) < 3:
        print("usage: run-virtio-balloon-test.py <iso> <disk.img> [--enable=0]", file=sys.stderr)
        return 2
    iso, disk = sys.argv[1], sys.argv[2]
    enable = True
    for a in sys.argv[3:]:
        if a in ("--enable=0", "--disable"):
            enable = False

    tmp = tempfile.mkdtemp(prefix="logit-balloon-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")

    args = [QEMU, "-cpu", "max", "-cdrom", iso,
            "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
            "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
            "-m", str(RAM_MB), "-smp", "4", "-accel", "tcg,thread=multi",
            "-vga", "none", "-device", "virtio-gpu-pci",
            "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
            "-serial", "file:" + serial, "-display", "none", "-no-reboot",
            "-qmp", "unix:%s,server,nowait" % sock]
    if enable:
        args += ["-device", "virtio-balloon-pci,id=bal0"]

    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fail = [False]

    def need(label, ok):
        print(("ok   " if ok else "FAIL ") + label)
        if not ok:
            fail[0] = True

    try:
        # QMP is live before the guest executes anything -- see the module
        # docstring for why this timing works without any runtime re-poll in
        # the driver.
        f = qmp_connect(sock)
        target_bytes = TARGET_TAKE_MB * 1024 * 1024
        want_guest_bytes = RAM_MB * 1024 * 1024 - target_bytes
        r = None
        if enable:
            r = qmp_cmd(f, "balloon", {"value": want_guest_bytes})

        deadline = time.time() + 90
        while time.time() < deadline:
            if "LOGIT_BOOT_OK" in read(serial):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        time.sleep(1.0)  # let a slow self-test line land after boot-ok

        log = read(serial)
        booted = "LOGIT_BOOT_OK" in log
        need("kernel reached LOGIT_BOOT_OK", booted)

        selftest = re.search(
            r"VIRTIO_BALLOON_SELFTEST \S+ inflated=(\d+) deflated=(\d+) "
            r"free_before=(\d+) free_after_inflate=(\d+) free_after_deflate=(\d+)", log)
        target_line = re.search(
            r"VIRTIO_BALLOON_TARGET \S+ target=(\d+) held=(\d+) "
            r"free_before=(\d+) free_after=(\d+)", log)

        qb = None
        if enable:
            qb = qmp_cmd(f, "query-balloon")

        if not enable:
            # NEGATIVE CONTROL: both positive assertions must be UNSATISFIABLE.
            ctrl_ok = booted and selftest is None and target_line is None
            if ctrl_ok:
                print("PASS (control): no virtio-balloon device -> no self-test "
                      "line and no target line, as required.")
                return 0
            print("CONTROL FAILED: expected no balloon evidence with no "
                  "virtio-balloon-pci device.")
            print("selftest match: %r" % (selftest.group(0) if selftest else None))
            print("target match:   %r" % (target_line.group(0) if target_line else None))
            print("----- serial output -----"); print(log); print("-------------------------")
            return 1

        need("VIRTIO_BALLOON_SELFTEST line present", selftest is not None)
        if selftest:
            inflated, deflated, fb, fai, fad = (int(x) for x in selftest.groups())
            print("  selftest: " + selftest.group(0))
            need("selftest inflated exactly 4 frames", inflated == 4)
            need("selftest deflated exactly what it inflated", deflated == inflated)
            need("free frames dropped by exactly the inflate count",
                 fb - fai == inflated)
            need("free frames recovered exactly after deflate", fad == fb)

        need("VIRTIO_BALLOON_TARGET line present", target_line is not None)
        if target_line:
            target_pages, held, fb2, fa2 = (int(x) for x in target_line.groups())
            print("  target:   " + target_line.group(0))
            want_pages = target_bytes // 4096
            need("target pages match what QMP requested (%d)" % want_pages,
                 target_pages == want_pages)
            need("held pages match the requested target (device had enough RAM)",
                 held == target_pages)
            need("guest's own accounting: free dropped by exactly the target",
                 fb2 - fa2 == target_pages)

        need("query-balloon reached the device", qb is not None and "return" in (qb or {}))
        if qb and "return" in qb:
            actual_bytes = qb["return"].get("actual")
            print("  query-balloon: %r" % qb["return"])
            # query-balloon speaks in logical VM size (vm_ram_size -
            # balloon_size); this driver's config.actual (== `held` pages)
            # speaks in the balloon's own page count. Converting through the
            # same subtraction QEMU's virtio_balloon_stat() does (see the
            # module docstring) is what makes this a real cross-check rather
            # than comparing two numbers in different units.
            expect_actual = RAM_MB * 1024 * 1024 - held * 4096 if target_line else None
            need("QMP's own view of config.actual (converted to logical VM "
                 "size) matches the guest's write -- two independent "
                 "implementations agreeing on the wire value",
                 expect_actual is not None and actual_bytes == expect_actual)

    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    if fail[0]:
        print()
        print("----- serial output -----")
        print(log)
        print("-------------------------")
        return 1
    print()
    print("PASS: virtio-balloon gives real physical frames back to the host, "
          "witnessed independently by the guest and by QEMU's own device model.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
