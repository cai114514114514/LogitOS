#!/usr/bin/env python3
"""Exercise amd-bootfb against QEMU's real 1002:5046 ati-vga model.

The gate correlates the firmware LFB with the emulated function's BAR, checks
that the device model bound amd-bootfb, and captures the retained scanout.  It
does not claim physical AMD hardware support or native GPU acceleration.
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


def firmware_path(env, candidates):
    configured = os.environ.get(env)
    if configured:
        path = Path(configured)
        return path if path.is_file() else None
    for candidate in candidates:
        path = Path(candidate)
        if path.is_file():
            return path
    return None


def frame_stats(path):
    frame = PPM(str(path))
    colours, lit, samples = set(), 0, 0
    for y in range(0, frame.h, 8):
        for x in range(0, frame.w, 8):
            rgb = frame.at(x, y)
            colours.add(rgb)
            lit += int(sum(rgb) > 24)
            samples += 1
    return frame.w, frame.h, len(colours), lit, samples


def run_one(kind, image, out, ovmf_code=None, ovmf_vars=None):
    run = out / kind
    run.mkdir(parents=True, exist_ok=True)
    serial = run / "serial.log"
    stderr = run / "qemu.stderr.log"
    sock = run / "qmp.sock"
    shot = run / "desktop.ppm"
    for stale in (serial, stderr, sock, shot, run / "result.json"):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    common = [
        QEMU, "-cpu", os.environ.get("AMD_BOOTFB_CPU", "SandyBridge"),
        "-m", "512M", "-smp", "2", "-accel", "tcg,thread=multi",
        "-net", "none", "-no-reboot", "-display", "none",
        "-serial", "file:" + str(serial),
        "-qmp", "unix:%s,server=on,wait=off" % sock,
        "-vga", "none", "-device", "ati-vga,vgamem_mb=16,xres=1280,yres=800",
    ]
    if kind == "bios":
        command = common + ["-cdrom", str(image), "-boot", "d"]
    else:
        vars_copy = run / "OVMF_VARS.fd"
        shutil.copyfile(ovmf_vars, vars_copy)
        command = common + [
            "-machine", "q35",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + str(ovmf_code),
            "-drive", "if=pflash,format=raw,file=" + str(vars_copy),
            "-device", "ich9-ahci,id=ahci0",
            "-drive", "file=%s,format=raw,if=none,id=esp0,file.locking=off" % image,
            "-device", "ide-hd,drive=esp0,bus=ahci0.0",
            "-boot", "order=c,menu=off",
        ]

    with stderr.open("wb") as err:
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=err)
    try:
        deadline = time.time() + float(os.environ.get("AMD_BOOTFB_BOOT_TIMEOUT", "300"))
        marker = None
        while time.time() < deadline:
            text = read(serial)
            if kind == "uefi" and "[efi] gop none" in text:
                raise RuntimeError("OVMF exposes no GOP for QEMU ati-vga")
            if "LOGIT_FB_FAIL" in text:
                raise RuntimeError("guest has no usable firmware framebuffer")
            marker = re.search(
                r"\[amd-bootfb\] ([^\n]*1002:5046[^\n]*"
                r"source=multiboot-lfb bar=(\d+)[^\n]*)", text)
            if marker and "[wm] desktop live" in text:
                break
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited before AMD passive display bind")
            time.sleep(0.2)
        else:
            raise RuntimeError("guest timed out before bind + desktop-live markers")

        text = read(serial)
        if not re.search(
                r"\[dev\] [^\n]*1002:5046 class=03\.00\.00 "
                r"[^\n]*driver=amd-bootfb", text):
            raise RuntimeError("device model did not publish driver=amd-bootfb")
        retained = ("[amd-bootfb] framebuffer retained; no modeset, GPU command, "
                    "BAR map, DMA, IRQ or MMIO write")
        if retained not in text:
            raise RuntimeError("passive/no-takeover marker missing")
        if "LOGIT_FB_FAIL" in text:
            raise RuntimeError("kernel rejected the firmware framebuffer")
        if kind == "uefi":
            for want in ("[efi] gop ", "[efi] ebs ok", "[efi] jump"):
                if want not in text:
                    raise RuntimeError("UEFI handoff marker missing: " + want)

        ui = Session(str(sock), timeout=20, serial=str(serial))
        ui.screendump(str(shot), settle=1.0)
        ui.f.close()
        ui.s.close()
        width, height, colours, lit, samples = frame_stats(shot)
        mode = re.search(r"bootfb=(\d+)x(\d+)", marker.group(1))
        if not mode or (width, height) != (int(mode.group(1)), int(mode.group(2))):
            raise RuntimeError("QMP scanout dimensions disagree with bound bootfb")
        if colours < 32 or lit * 4 < samples:
            raise RuntimeError("post-bind scanout is blank/flat (%d colours, %d/%d lit)" %
                               (colours, lit, samples))

        result = {
            "firmware": kind,
            "evidence": "qemu-ati-vga-1002:5046",
            "bar": int(marker.group(2)),
            "width": width,
            "height": height,
            "sample_colours": colours,
            "lit_samples": lit,
            "samples": samples,
            "image": str(image),
            "image_sha256": sha256(image),
            "physical_amd_gpu_verified": False,
            "native_gpu_acceleration_verified": False,
        }
        (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("PASS %-4s QEMU ati-vga bind + retained %dx%d scanout (%d sampled colours)" %
              (kind, width, height, colours))
        return result
    except Exception:
        print("--- %s serial tail ---" % kind, file=sys.stderr)
        print(read(serial)[-12000:], file=sys.stderr)
        print("--- %s QEMU stderr ---" % kind, file=sys.stderr)
        print(read(stderr)[-4000:], file=sys.stderr)
        raise
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True)
    parser.add_argument("--esp")
    parser.add_argument("--out", required=True)
    parser.add_argument("--firmware", choices=("bios", "uefi", "both"),
                        default="both")
    args = parser.parse_args()
    iso = Path(args.iso).resolve()
    esp = Path(args.esp).resolve() if args.esp else None
    out = Path(args.out).resolve()
    if not iso.is_file():
        parser.error("--iso must exist")
    if args.firmware in ("uefi", "both") and (not esp or not esp.is_file()):
        parser.error("--esp must exist for UEFI")
    if not shutil.which(QEMU):
        parser.error("QEMU not found: " + QEMU)

    out.mkdir(parents=True, exist_ok=True)
    results = []
    if args.firmware in ("bios", "both"):
        results.append(run_one("bios", iso, out))
    if args.firmware in ("uefi", "both"):
        code = firmware_path("OVMF_CODE", [
            "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
            "/usr/share/OVMF/OVMF_CODE_4M.fd",
        ])
        vars_image = firmware_path("OVMF_VARS_SRC", [
            "/opt/homebrew/share/qemu/edk2-i386-vars.fd",
            "/usr/share/OVMF/OVMF_VARS_4M.fd",
        ])
        if not code or not vars_image:
            parser.error("OVMF unavailable; set OVMF_CODE and OVMF_VARS_SRC")
        results.append(run_one("uefi", esp, out, code, vars_image))
    summary = {
        "scope": "QEMU ati-vga boot framebuffer integration; not physical AMD hardware",
        "runs": results,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("AMD_BOOTFB_GUEST: %s QEMU ati-vga path passed; physical AMD GPU unverified" %
          args.firmware.upper())


if __name__ == "__main__":
    main()
