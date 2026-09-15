#!/usr/bin/env python3
"""Prove LogitOS's RV100 2D canary against QEMU's ATI device model.

This gate deliberately names its proof boundary.  QEMU exposes the real PCI
identity 1002:5159 and executes ATI MMIO commands, but it is not a physical AMD
board.  The serial assertions prove the guest's readback/restore result while
QEMU's ati_mm_* trace independently proves that the device model received the
2D command-register writes.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "qmp"))
from qmp_ui import PPM, Session  # noqa: E402


QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
DEVICE = "ati-vga,model=rv100,vgamem_mb=16,xres=1280,yres=800"
DEVICE_RE = re.compile(
    r"\[dev\] (?P<bdf>\S+) 1002:5159 class=03\.00\.00 "
    r"[^\r\n]*\bdriver=amd-bootfb\b"
)
ACCEL_RE = re.compile(
    r"^\[amd-accel\] (?P<fields>[^\r\n]*\bfamily=RV100\b"
    r"[^\r\n]*\bpci=5159\b[^\r\n]*\bstage=active\b[^\r\n]*)$",
    re.MULTILINE,
)


def read(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_fields(line):
    return dict(re.findall(r"\b([a-z][a-z0-9_-]*)=([^\s]+)", line))


def frame_stats(path):
    frame = PPM(str(path))
    colours = set()
    lit = 0
    samples = 0
    for y in range(0, frame.h, 8):
        for x in range(0, frame.w, 8):
            rgb = frame.at(x, y)
            colours.add(rgb)
            lit += int(sum(rgb) > 24)
            samples += 1
    return frame.w, frame.h, len(colours), lit, samples


def qemu_version():
    completed = subprocess.run(
        [QEMU, "-version"], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=True,
    )
    return completed.stdout.splitlines()[0].strip()


def find_qmp_pci(node, vendor, device):
    if isinstance(node, dict):
        ident = node.get("id")
        if (isinstance(ident, dict) and ident.get("vendor") == vendor and
                ident.get("device") == device):
            return node
        for value in node.values():
            found = find_qmp_pci(value, vendor, device)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find_qmp_pci(value, vendor, device)
            if found is not None:
                return found
    return None


def require_accel_fields(fields):
    expected = {
        "family": "RV100",
        "pci": "5159",
        "stage": "active",
        "commands": "2",
        "engine": "rv100-2d-canary-passed",
        "desktop": "cpu",
        "fill": "ok",
        "copy": "ok",
        "restore": "ok",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise RuntimeError(
                "AMD acceleration marker lacks %s=%s (got %r)" %
                (key, value, fields.get(key))
            )
    for key in ("vram", "scanout", "canary", "reads", "writes"):
        if key not in fields:
            raise RuntimeError("AMD acceleration marker lacks " + key)
    if int(fields["vram"], 0) != 16 * 1024 * 1024:
        raise RuntimeError("driver did not observe the configured 16 MiB VRAM")
    if int(fields["canary"], 0) >= int(fields["vram"], 0):
        raise RuntimeError("canary offset is outside reported VRAM")
    if int(fields["reads"], 0) < 3 or int(fields["writes"], 0) < 2:
        raise RuntimeError("operation counters cannot cover fill/copy/readback/restore")


def trace_evidence(trace, fields):
    lines = read(trace).splitlines()
    # Release QEMU builds compile ati_reg_name() to an empty string, so match
    # the public register offsets as well as the optional debug-build names.
    memsize = [line for line in lines if re.search(
        r"\bati_mm_read\s+\d+\s+0x0*f8\b|\bCNFG_MEMSIZE\b", line
    )]
    status = [line for line in lines if re.search(
        r"\bati_mm_read\s+\d+\s+0x0*(?:e40|1740)\b|"
        r"\b(?:GUI_STAT|RBBM_STATUS)\b", line
    )]
    def writes(offset, value):
        return [line for line in lines if re.search(
            r"\bati_mm_write\s+\d+\s+0x0*%x\b.*<-\s+0x0*%x\b" %
            (offset, value), line, re.IGNORECASE)]

    # These values are unique to LogitOS's 13x7 off-screen canary.  Checking
    # the setup values ties the trace to the guest self-test instead of
    # accidentally counting a firmware write to the same trigger register.
    trigger_value = 0x0007000D
    triggers = writes(0x143C, trigger_value)
    fill_master = writes(0x146C, 0x52F006DE)
    copy_master = writes(0x146C, 0x52CC36FF)
    canary = int(fields["canary"], 0)
    src_pitch = (canary >> 10) | (1 << 22)
    dst_pitch = ((canary + 1024) >> 10) | (1 << 22)
    src_setup = writes(0x1428, src_pitch)
    dst_src_setup = writes(0x142C, src_pitch)
    dst_copy_setup = writes(0x142C, dst_pitch)
    cache_flush = writes(0x1714, 0xF)
    if not memsize:
        raise RuntimeError("QEMU trace has no CNFG_MEMSIZE read")
    if not status:
        raise RuntimeError("QEMU trace has no engine-status read")
    if len(triggers) < 2:
        raise RuntimeError(
            "QEMU trace has fewer than two LogitOS 13x7 trigger writes"
        )
    if not fill_master or not copy_master:
        raise RuntimeError("QEMU trace lacks exact fill/copy GUI master setup")
    if not src_setup or not dst_src_setup or not dst_copy_setup:
        raise RuntimeError("QEMU trace lacks exact off-screen pitch/offset setup")
    if len(cache_flush) < 3:
        raise RuntimeError("QEMU trace lacks initial/fill/copy destination-cache flushes")
    return {
        "cnfg_memsize_reads": len(memsize),
        "engine_status_reads": len(status),
        "canary_trigger_value": hex(trigger_value),
        "canary_trigger_writes": len(triggers),
        "fill_master_writes": len(fill_master),
        "copy_master_writes": len(copy_master),
        "cache_flush_writes": len(cache_flush),
        "dst_height_width_tail": triggers[-2:],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=float,
                        default=float(os.environ.get("AMD_RV100_TIMEOUT", "300")))
    args = parser.parse_args()

    iso = Path(args.iso).resolve()
    out = Path(args.out).resolve()
    if not iso.is_file():
        parser.error("--iso must exist")
    if not shutil.which(QEMU):
        parser.error("QEMU not found: " + QEMU)
    out.mkdir(parents=True, exist_ok=True)

    serial = out / "serial.log"
    stderr = out / "qemu.stderr.log"
    trace = out / "ati-mmio.trace"
    sock = out / "qmp.sock"
    shot = out / "desktop.ppm"
    result_path = out / "result.json"
    for stale in (serial, stderr, trace, sock, shot, result_path):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    command = [
        QEMU,
        "-cpu", os.environ.get("AMD_RV100_CPU", "SandyBridge"),
        "-m", "512M",
        "-smp", "2",
        "-accel", "tcg,thread=multi",
        "-net", "none",
        "-no-reboot",
        "-display", "none",
        "-serial", "file:" + str(serial),
        "-qmp", "unix:%s,server=on,wait=off" % sock,
        "-vga", "none",
        "-device", DEVICE,
        "-trace", "enable=ati_mm_*,file=" + str(trace),
        "-cdrom", str(iso),
        "-boot", "d",
    ]

    proc = None
    try:
        with stderr.open("wb") as err:
            proc = subprocess.Popen(
                command, stdout=subprocess.DEVNULL, stderr=err
            )
        deadline = time.monotonic() + args.timeout
        device_match = None
        accel_match = None
        while time.monotonic() < deadline:
            text = read(serial)
            device_match = DEVICE_RE.search(text)
            accel_match = ACCEL_RE.search(text)
            if device_match and accel_match and "[wm] desktop live" in text:
                break
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited before RV100 acceleration evidence")
            time.sleep(0.2)
        else:
            raise RuntimeError(
                "guest timed out before PCI bind + active canary + desktop markers"
            )

        text = read(serial)
        if re.search(r"^\[amd-accel\][^\r\n]*\bstage=blocked\b",
                     text, re.MULTILINE):
            raise RuntimeError("RV100 acceleration entered a blocked state")
        if "LOGIT_FB_FAIL" in text:
            raise RuntimeError("guest has no usable firmware framebuffer")
        bootfb = re.search(
            r"\[amd-bootfb\] [^\r\n]*1002:5159[^\r\n]*"
            r"source=multiboot-lfb[^\r\n]*", text
        )
        if not bootfb:
            raise RuntimeError("amd-bootfb did not prove LFB ownership")
        boot_mode = re.search(r"\bbootfb=(\d+)x(\d+)\b", bootfb.group(0))
        if not boot_mode:
            raise RuntimeError("amd-bootfb marker has no bootfb dimensions")
        expected_width, expected_height = map(int, boot_mode.groups())

        fields = parse_fields(accel_match.group("fields"))
        require_accel_fields(fields)

        ui = Session(str(sock), timeout=20, serial=str(serial))
        try:
            qmp_pci = ui.cmd({"execute": "query-pci"})
            qmp_device = find_qmp_pci(qmp_pci, 0x1002, 0x5159)
            if qmp_device is None:
                raise RuntimeError("QMP query-pci has no 1002:5159 function")
            ui.screendump(str(shot), settle=1.0)
        finally:
            ui.f.close()
            ui.s.close()
        width, height, colours, lit, samples = frame_stats(shot)
        if (width, height) != (expected_width, expected_height):
            raise RuntimeError(
                "QMP scanout is %dx%d, bootfb marker reports %dx%d" %
                (width, height, expected_width, expected_height)
            )
        if colours < 32 or lit * 4 < samples:
            raise RuntimeError(
                "post-canary scanout is blank/flat (%d colours, %d/%d lit)" %
                (colours, lit, samples)
            )

        # Stop the emulator before parsing the trace so its buffered event
        # stream is complete.
        proc.terminate()
        proc.wait(timeout=5)
        trace_result = trace_evidence(trace, fields)

        result = {
            "passed": True,
            "scope": "QEMU ATI RV100 2D command path; physical AMD hardware excluded",
            "firmware": "bios",
            "qemu_version": qemu_version(),
            "qemu_device": DEVICE,
            "pci_identity": "1002:5159",
            "pci_bdf": device_match.group("bdf"),
            "qmp_pci_identity": qmp_device.get("id"),
            "driver": "amd-bootfb",
            "bootfb_marker": bootfb.group(0),
            "accel_fields": fields,
            "qemu_trace": trace_result,
            "scanout": {
                "width": width,
                "height": height,
                "sample_colours": colours,
                "lit_samples": lit,
                "samples": samples,
            },
            "iso": str(iso),
            "iso_sha256": sha256(iso),
            "qemu_device_model_verified": True,
            "rv100_fill_copy_canary_verified": True,
            "canary_original_bytes_restored": True,
            "physical_amd_gpu_verified": False,
            "modern_amd_gpu_acceleration_verified": False,
        }
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        print(
            "AMD_RV100_ACCEL_GUEST: PASS 1002:5159 fill/copy canary + "
            "restore; QEMU MMIO trace + non-empty %dx%d scanout; "
            "physical AMD GPU unverified" % (width, height)
        )
    except Exception:
        print("--- RV100 serial tail ---", file=sys.stderr)
        print(read(serial)[-16000:], file=sys.stderr)
        print("--- RV100 QEMU trace tail ---", file=sys.stderr)
        print(read(trace)[-8000:], file=sys.stderr)
        print("--- RV100 QEMU stderr ---", file=sys.stderr)
        print(read(stderr)[-4000:], file=sys.stderr)
        raise
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    main()
