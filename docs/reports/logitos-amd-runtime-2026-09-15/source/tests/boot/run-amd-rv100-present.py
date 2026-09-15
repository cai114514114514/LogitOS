#!/usr/bin/env python3
"""Exercise AMD desktop presentation in QEMU, with an engine-disabled control.

The earlier gate stops at an off-screen startup canary.  This gate observes
commands whose destination is the visible scanout, then drives the guest's
software pointer and requires presentation counters to advance.  A second ISO
built with AMD_PRESENT_DISABLE=1 must render the same desktop without any such
commands; it is passed through the very same positive marker assertion and
must fail there.  Host duration is only a timeout, never a speed measurement.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "amd_canary_gate", Path(__file__).with_name("run-amd-rv100-accel.py"))
canary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(canary)
sys.path.insert(0, str(ROOT / "tests" / "qmp"))
from qmp_ui import PPM, Session, configure  # noqa: E402

PRESENT = re.compile(r"^\[amd-present\] ([^\r\n]*\bcompleted=\d+[^\r\n]*)\r?$",
                     re.MULTILINE)
# A runtime timeout quarantines outstanding GPU work; earlier successful
# frames must never turn that later failure into a passing guest run.
FAILURE = re.compile(
    r"^\[amd-(?:accel|present)\][^\r\n]*"
    r"\b(?:stage=blocked|failed=1|quarantined=1)\b", re.MULTILINE)
WRITE = re.compile(r"\bati_mm_write\s+\d+\s+0x([0-9a-f]+)\b.*<-\s+0x([0-9a-f]+)\b",
                   re.IGNORECASE)


def samples(text):
    return [canary.parse_fields(m.group(1)) for m in PRESENT.finditer(text)]


def require_runtime(rows, baseline=0):
    if not rows:
        raise RuntimeError("runtime-present-missing")
    last = rows[-1]
    for name in ("completed", "pixels", "commands", "upload_bytes", "scanout",
                 "scratch", "pitch", "rects"):
        if name not in last:
            raise RuntimeError("runtime marker lacks " + name)
        int(last[name], 0)
    if int(last["completed"], 0) < max(2, baseline + 1):
        raise RuntimeError("runtime-present-did-not-advance")
    if int(last["commands"], 0) <= 2 or int(last["upload_bytes"], 0) <= 0:
        raise RuntimeError("runtime marker has no post-canary upload/command")
    if int(last["pixels"], 0) <= 0 or int(last["rects"], 0) <= 0:
        raise RuntimeError("runtime marker has no presented pixels/rectangles")
    if not any(row.get("verified") == "1" for row in rows):
        raise RuntimeError("runtime marker lacks first-frame scanout readback")
    completions = [int(row["completed"], 0) for row in rows]
    if any(a >= b for a, b in zip(completions, completions[1:])):
        raise RuntimeError("runtime completion counters did not increase")
    return last


def trace_runtime(path, fields, width, height):
    """Reconstruct each trigger's register state, rather than count any blit.

    Firmware and the canary write this same engine.  A runtime command must
    have the guest-published scanout destination, full-frame pitch, in-bounds
    coordinates, and (for copy) the published off-screen source.  This rejects
    a driver that repeats its canary forever without ever showing its output.
    """
    scanout = int(fields["scanout"], 0)
    scratch = int(fields["scratch"], 0)
    pitch = int(fields["pitch"], 0)
    if pitch < width * 4 or pitch % 64 or scratch < scanout + pitch * height:
        raise RuntimeError("invalid runtime scratch/scanout layout")
    dst_pitch = (scanout >> 10) | ((pitch >> 6) << 22)
    src_pitch = (scratch >> 10) | ((pitch >> 6) << 22)
    registers, copies, fills, flushes = {}, [], [], 0
    for line in canary.read(path).splitlines():
        match = WRITE.search(line)
        if not match:
            continue
        off, value = (int(part, 16) for part in match.groups())
        registers[off] = value
        if off == 0x1714 and value == 0xf:
            flushes += 1
        if off != 0x143c or registers.get(0x142c) != dst_pitch:
            continue
        w, h = value & 0xffff, value >> 16
        xy = registers.get(0x1438, 0)
        x, y = xy & 0xffff, xy >> 16
        if not w or not h or x + w > width or y + h > height:
            raise RuntimeError("trace submitted an out-of-scanout runtime rectangle")
        entry = {"x": x, "y": y, "width": w, "height": h,
                 "trigger": hex(value), "dst_pitch_offset": hex(dst_pitch)}
        master = registers.get(0x146c)
        if master == 0x52cc36ff and registers.get(0x1428) == src_pitch:
            if registers.get(0x1434) != xy:
                raise RuntimeError("runtime upload does not use matching scratch coordinates")
            entry["src_pitch_offset"] = hex(src_pitch)
            copies.append(entry)
        elif master == 0x52f006de:
            fills.append(entry)
    if len(copies) < 2:
        raise RuntimeError("trace lacks repeated scratch-to-visible-scanout GPU copies")
    if flushes < len(copies) + len(fills) + 3:
        raise RuntimeError("trace lacks cache completion flushes for runtime commands")
    if not any(int(row["trigger"], 16) != 0x7000d for row in copies):
        raise RuntimeError("trace has only canary-sized copies")
    return {"visible_copy_commands": len(copies), "visible_fill_commands": len(fills),
            "cache_flush_writes": flushes, "copy_samples": copies[:6] + copies[-2:],
            "source_scratch": scratch, "destination_scanout": scanout, "pitch": pitch}


def visible_trigger_count(path, scanout, pitch):
    """CPU control must have no visible 2D command, although canary still runs."""
    expected = (scanout >> 10) | ((pitch >> 6) << 22)
    registers, count, canary_triggers = {}, 0, 0
    for line in canary.read(path).splitlines():
        match = WRITE.search(line)
        if match:
            off, value = (int(part, 16) for part in match.groups())
            registers[off] = value
            if off == 0x143c and value == 0x7000d and registers.get(0x142c) != expected:
                canary_triggers += 1
            if (off == 0x143c and canary_triggers >= 2 and
                    registers.get(0x142c) == expected):
                count += 1
    return count


def wait_for(proc, deadline, predicate, message):
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if proc.poll() is not None:
            raise RuntimeError("QEMU exited: " + message)
        time.sleep(0.2)
    raise RuntimeError(message)


def run_guest(iso, out, timeout, cpu_only):
    out.mkdir(parents=True, exist_ok=True)
    paths = {name: out / value for name, value in {
        "serial": "serial.log", "trace": "ati-mmio.trace", "stderr": "qemu.stderr.log",
        "sock": "qmp.sock", "before": "before.ppm", "after": "after.ppm"}.items()}
    for path in paths.values():
        path.unlink(missing_ok=True)
    before_hash = canary.sha256(iso)
    cmd = [canary.QEMU, "-cpu", os.environ.get("AMD_RV100_CPU", "SandyBridge"),
           "-m", "512M", "-smp", "2", "-accel", "tcg,thread=multi", "-net", "none",
           "-no-reboot", "-display", "none", "-serial", "file:" + str(paths["serial"]),
           "-qmp", "unix:%s,server=on,wait=off" % paths["sock"], "-vga", "none",
           "-device", canary.DEVICE, "-rtc", "base=2026-09-15T12:00:00,clock=vm",
           "-trace", "enable=ati_mm_*,file=" + str(paths["trace"]),
           "-cdrom", str(iso), "-boot", "d"]
    proc, ui = None, None
    try:
        with paths["stderr"].open("wb") as err:
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)
        deadline = time.monotonic() + timeout
        def ready():
            text = canary.read(paths["serial"])
            return (text if canary.DEVICE_RE.search(text) and
                    canary.ACCEL_RE.search(text) and "[wm] desktop live" in text else None)
        text = wait_for(proc, deadline, ready, "timeout before canary and live desktop")
        startup = canary.parse_fields(canary.ACCEL_RE.search(text).group("fields"))
        canary.require_accel_fields(startup)
        mode = re.search(r"\[amd-bootfb\][^\r\n]*bootfb=(\d+)x(\d+)", text)
        if not mode:
            raise RuntimeError("missing firmware scanout dimensions")
        width, height = map(int, mode.groups())
        configure(width, height)
        ui = Session(str(paths["sock"]), timeout=20, serial=str(paths["serial"]))
        ui.s.settimeout(20)
        pci = canary.find_qmp_pci(ui.cmd({"execute": "query-pci"}), 0x1002, 0x5159)
        if pci is None:
            raise RuntimeError("QMP has no real 1002:5159 device")
        ui.screendump(str(paths["before"]), settle=0.5)
        initial_rows = samples(canary.read(paths["serial"]))
        baseline = int(initial_rows[-1]["completed"], 0) if initial_rows else 0
        pointer_before = ui.guest_pointer()
        if pointer_before is not None:
            ui.cur = list(pointer_before)
        moves = []
        # Keep the pointer clear of hot corners and dock/application launchers.
        # An idle menu-clock also damages rectangles, so counters are evidence
        # of repeated real presentation during input, not a per-input latency.
        for n in range(12):
            x = width // 4 if n % 2 == 0 else 3 * width // 4
            y = height // 3 if n % 3 == 0 else height // 2
            ui.goto(x, y, settle=0.3)
            moves.append([x, y])
        ui.goto(width // 2, height // 2, settle=0.5)
        pointer_after = wait_for(proc, deadline,
            lambda: ui.guest_pointer(), "guest never reported input pointer")
        if len(set(re.findall(r"\[wm\] ptr (\d+) (\d+)",
                              canary.read(paths["serial"])))) < 2:
            raise RuntimeError("QMP movement did not reach guest pointer")
        if not cpu_only:
            def advanced():
                rows = samples(canary.read(paths["serial"]))
                return rows if rows and int(rows[-1]["completed"], 0) > max(1, baseline) else None
            wait_for(proc, deadline, advanced, "runtime-present-did-not-advance")
        ui.screendump(str(paths["after"]), settle=0.5)
        stats = canary.frame_stats(paths["after"])
        if stats[:2] != (width, height) or stats[2] < 32 or stats[3] * 4 < stats[4]:
            raise RuntimeError("runtime desktop scanout is empty or has wrong dimensions")
        ui.f.close()
        ui.s.close()
        ui = None
        proc.terminate()
        proc.wait(timeout=5)
        text = canary.read(paths["serial"])
        if FAILURE.search(text) or "LOGIT_FB_FAIL" in text:
            raise RuntimeError("AMD path reported failure")
        startup_trace = canary.trace_evidence(paths["trace"], startup)
        rows = samples(text)
        result = {"passed": True, "mode": "cpu-control" if cpu_only else "gpu-present",
                  "iso": str(iso), "iso_sha256": before_hash,
                  "qemu_version": canary.qemu_version(), "qemu_device": canary.DEVICE,
                  "pci_identity": pci.get("id"), "startup_canary": startup,
                  "startup_trace": startup_trace, "runtime_markers": rows,
                  "completed_before_input": baseline, "input_moves": moves,
                  "pointer_before": pointer_before, "pointer_after": pointer_after,
                  "scanout": dict(zip(("width", "height", "sample_colours", "lit_samples", "samples"), stats)),
                  "physical_amd_gpu_verified": False,
                  "modern_amd_gpu_acceleration_verified": False,
                  "gpu_shader_compositing_verified": False,
                  "performance_speedup_measured": False}
        if cpu_only:
            try:
                require_runtime(rows)
            except RuntimeError as exc:
                if str(exc) != "runtime-present-missing":
                    raise
                result["positive_assertion_rejected"] = str(exc)
            else:
                raise RuntimeError("CPU control unexpectedly passed runtime positive assertion")
            count = visible_trigger_count(paths["trace"], int(startup["scanout"], 0), width * 4)
            if count:
                raise RuntimeError("CPU control sent %d visible GPU commands" % count)
            result["visible_gpu_commands"] = count
        else:
            last = require_runtime(rows, baseline)
            result["runtime_trace"] = trace_runtime(paths["trace"], last, width, height)
            result["rv100_visible_gpu_presentation_verified"] = True
        if canary.sha256(iso) != before_hash:
            raise RuntimeError("ISO changed during guest verification")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
    except Exception:
        print(canary.read(paths["serial"])[-10000:], file=sys.stderr)
        print(canary.read(paths["stderr"])[-2000:], file=sys.stderr)
        raise
    finally:
        if ui is not None:
            ui.f.close()
            ui.s.close()
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def compare_frames(gpu_path, cpu_path):
    a, b = PPM(str(gpu_path)), PPM(str(cpu_path))
    if (a.w, a.h) != (b.w, b.h):
        raise RuntimeError("CPU and GPU scanout dimensions differ")
    # Both renders have a fixed RTC and finish with the pointer at the centre.
    # Record raw whole-frame equality as well as an interior sample; exclude
    # only the menu clock/dock, whose animation phase can differ between boots.
    equal = total = 0
    for y in range(a.h // 6, 5 * a.h // 6, 2):
        for x in range(a.w // 10, 9 * a.w // 10, 2):
            total += 1
            equal += a.at(x, y) == b.at(x, y)
    ratio = equal / total
    if ratio < 0.99:
        raise RuntimeError("CPU/GPU desktop interior differs: %.6f match" % ratio)
    return {"whole_frame_sha256_equal": canary.sha256(gpu_path) == canary.sha256(cpu_path),
            "interior_equal_samples": equal, "interior_samples": total,
            "interior_match_ratio": ratio,
            "interior_bounds_fraction": {"x": [0.1, 0.9], "y": [1 / 6, 5 / 6]},
            "clock_and_dock_excluded_from_interior": True}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True)
    parser.add_argument("--cpu-iso", help="separate AMD_PRESENT_DISABLE=1 control ISO")
    parser.add_argument("--out", required=True)
    parser.add_argument("--expect-cpu-only", action="store_true")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    iso, out = Path(args.iso).resolve(), Path(args.out).resolve()
    if not iso.is_file() or not shutil.which(canary.QEMU):
        parser.error("ISO or QEMU is missing")
    if args.cpu_iso and args.expect_cpu_only:
        parser.error("--cpu-iso and --expect-cpu-only are mutually exclusive")
    cpu_iso = Path(args.cpu_iso).resolve() if args.cpu_iso else None
    if cpu_iso and (not cpu_iso.is_file() or canary.sha256(cpu_iso) == canary.sha256(iso)):
        parser.error("CPU control must be a different, existing ISO")
    result = run_guest(iso, out / "gpu" if cpu_iso else out,
                       args.timeout, args.expect_cpu_only)
    if cpu_iso:
        control = run_guest(cpu_iso, out / "cpu", args.timeout, True)
        result = {"passed": True, "gpu": result, "cpu_control": control,
                  "frame_comparison": compare_frames(out / "gpu/after.ppm", out / "cpu/after.ppm")}
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("AMD_RV100_PRESENT_GUEST: PASS " +
          ("CPU control rejects missing runtime presentation" if args.expect_cpu_only else
           "visible GPU copies + completed desktop presents + scanout readback") +
          ("; CPU control and desktop parity passed" if cpu_iso else ""))


if __name__ == "__main__":
    main()
