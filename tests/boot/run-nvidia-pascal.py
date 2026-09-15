#!/usr/bin/env python3
"""Boot the passive display driver through BIOS LFB and UEFI GOP.

QEMU does not emulate a GP107.  The isolated PASCALVERIFY kernel therefore
adds 1234:1111 (QEMU stdvga) as an unmistakably TEST-ONLY match.  This guest
gate proves the linker-section declaration reaches dev_probe_all(), the probe
correlates the actual Multiboot framebuffer with the PCI BAR that contains it,
and the desktop remains visible after the passive bind.  Exact NVIDIA IDs and
the zero-write property are established by the host gate; only a real GTX 1050
can establish physical hardware operation.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
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


def firmware_path(env, candidates):
    value = os.environ.get(env)
    if value:
        p = Path(value)
        return p if p.is_file() else None
    for value in candidates:
        p = Path(value)
        if p.is_file():
            return p
    return None


def frame_stats(path):
    p = PPM(str(path))
    colours, lit, samples = set(), 0, 0
    # Sampling every eighth row/column keeps this bounded while still crossing
    # every large desktop surface and the wallpaper gradient.
    for y in range(0, p.h, 8):
        for x in range(0, p.w, 8):
            rgb = p.at(x, y)
            colours.add(rgb)
            lit += int(sum(rgb) > 24)
            samples += 1
    return p.w, p.h, len(colours), lit, samples


def run_one(kind, image, out, ovmf_code=None, ovmf_vars=None):
    run = out / kind
    run.mkdir(parents=True, exist_ok=True)
    serial = run / "serial.log"
    stderr = run / "qemu.stderr.log"
    sock = run / "qmp.sock"
    shot = run / "desktop.ppm"

    # QEMU opens its serial file after Popen returns.  Without clearing a prior
    # run first, the polling loop can momentarily see old success markers and
    # capture the new VM before it has selected the requested video mode.
    for stale in (serial, stderr, sock, shot, run / "result.json"):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    common = [
        QEMU, "-cpu", os.environ.get("NVIDIA_PASCAL_CPU", "SandyBridge"),
        "-m", "512M", "-smp", "2",
        "-accel", "tcg,thread=multi", "-net", "none", "-no-reboot",
        "-display", "none", "-serial", "file:" + str(serial),
        "-qmp", "unix:%s,server=on,wait=off" % sock,
        "-vga", "none", "-device", "VGA,vgamem_mb=16,xres=1280,yres=800",
    ]
    temp_vars = None
    if kind == "bios":
        command = common + ["-cdrom", str(image), "-boot", "d"]
    else:
        temp_vars = run / "OVMF_VARS.fd"
        shutil.copyfile(ovmf_vars, temp_vars)
        command = common + [
            "-machine", "q35",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + str(ovmf_code),
            "-drive", "if=pflash,format=raw,file=" + str(temp_vars),
            "-device", "ich9-ahci,id=ahci0",
            "-drive", "file=%s,format=raw,if=none,id=esp0,file.locking=off" % image,
            "-device", "ide-hd,drive=esp0,bus=ahci0.0",
            "-boot", "order=c,menu=off",
        ]

    with stderr.open("wb") as err:
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=err)
    try:
        deadline = time.time() + float(os.environ.get("NVIDIA_PASCAL_BOOT_TIMEOUT", "300"))
        marker = None
        while time.time() < deadline:
            text = read(serial)
            marker = re.search(
                r"\[nv-bootfb\] ([^\n]*1234:1111[^\n]*TEST-ONLY[^\n]*"
                r"source=multiboot-lfb bar=(\d+)[^\n]*)", text)
            if marker and "[wm] desktop live" in text:
                break
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited before passive display bind")
            time.sleep(0.2)
        else:
            raise RuntimeError("guest timed out before bind + desktop-live markers")

        text = read(serial)
        if not re.search(r"\[dev\] [^\n]*1234:1111 class=03\.00\.00 [^\n]*driver=nv-bootfb", text):
            raise RuntimeError("device model did not publish driver=nv-bootfb")
        if "[nv-bootfb] framebuffer retained; no GPU MMIO, modeset, clocks, DMA, IRQ or 3D" not in text:
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
            "evidence": "synthetic-qemu-stdvga",
            "bar": int(marker.group(2)),
            "width": width,
            "height": height,
            "sample_colours": colours,
            "lit_samples": lit,
            "samples": samples,
            "physical_gtx1050_verified": False,
        }
        (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("PASS %-4s synthetic bind + retained %dx%d scanout (%d sampled colours)" %
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--esp", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    iso, esp, out = Path(args.iso).resolve(), Path(args.esp).resolve(), Path(args.out).resolve()
    if not iso.is_file() or not esp.is_file():
        ap.error("--iso and --esp must exist")
    if not shutil.which(QEMU):
        ap.error("QEMU not found: " + QEMU)

    code = firmware_path("OVMF_CODE", [
        "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
    ])
    vars_image = firmware_path("OVMF_VARS_SRC", [
        "/opt/homebrew/share/qemu/edk2-i386-vars.fd",
        "/usr/share/OVMF/OVMF_VARS_4M.fd",
    ])
    if not code or not vars_image:
        ap.error("OVMF unavailable; set OVMF_CODE and OVMF_VARS_SRC to run the UEFI half")

    out.mkdir(parents=True, exist_ok=True)
    try:
        (out / "summary.json").unlink()
    except FileNotFoundError:
        pass
    results = [run_one("bios", iso, out),
               run_one("uefi", esp, out, code, vars_image)]
    (out / "summary.json").write_text(json.dumps({
        "scope": "synthetic boot-framebuffer integration; not physical GP107",
        "runs": results,
    }, indent=2) + "\n")
    print("NVIDIA_PASCAL_GUEST: BIOS + UEFI synthetic framebuffer paths passed; physical GTX 1050 unverified")


if __name__ == "__main__":
    main()
