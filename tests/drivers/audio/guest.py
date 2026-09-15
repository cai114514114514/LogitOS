#!/usr/bin/env python3
"""Capture actual sndtest output from one emulated card, retaining all evidence.

QEMU 11 can leave RIFF sizes zero even after QMP quit. The bounded compatibility
path records that fact and retains raw bytes beside a playable header-only copy.
No result is accepted until QEMU has exited and both the guest marker and PCM
oracle pass.
Host deadlines only bound hangs: pitch and duration are measured in PCM frames,
never inferred from host runtime on this concurrently used TCG machine.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().with_suffix("")))
from oracle import CHECKER, RATES, inspect, self_test


def digest(path):
    result = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def record(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def qemu_value(path):
    # QEMU keyval escaping is separate from shell quoting. No shell is used.
    return str(path).replace(",", ",,")


def run(args, output):
    result = {"passed": False, "device": args.device, "errors": []}
    process = None
    monitor_directory = tempfile.TemporaryDirectory(prefix="audio-qmp-")
    monitor_path = Path(monitor_directory.name) / "qmp.sock"
    try:
        controls = self_test(output / "oracle")
        record(output / "oracle.json", controls)
        if not controls["passed"]:
            raise RuntimeError("oracle positive/negative prerequisites failed")
        result["oracle_sources"] = {
            str(path): digest(path) for path in
            (Path(__file__).resolve(), Path(__file__).resolve().with_suffix("") / "oracle.py",
             CHECKER)
        }
        original_iso = args.iso.resolve(strict=True)
        original_disk = args.disk.resolve(strict=True)
        iso = output / "input.iso"
        # Freeze the kernel artifact because other agents may relink its source
        # path during a run. Disk writes use QEMU's private snapshot overlay.
        shutil.copyfile(original_iso, iso)
        disk_hash = digest(original_disk)
        result["inputs"] = {"iso": str(original_iso), "iso_sha256": digest(iso),
                            "disk": str(original_disk), "disk_sha256": disk_hash,
                            "disk_writes": "QEMU snapshot overlay"}
        qemu = shutil.which(os.environ.get("QEMU", "qemu-system-x86_64"))
        if not qemu:
            raise RuntimeError("SKIP: qemu-system-x86_64 unavailable; rerun this command with QEMU installed")
        result["qemu_version"] = subprocess.check_output([qemu, "--version"], text=True).splitlines()[0]
        rate = RATES[args.device]
        wav = output / "capture.wav"
        audio = f"wav,id=snd0,path={qemu_value(wav)},out.frequency={rate},out.channels=2,out.format=s16"
        devices = (["-device", "intel-hda", "-device", "hda-output,audiodev=snd0"]
                   if args.device == "intel-hda" else ["-device", f"{args.device},audiodev=snd0"])
        command = [qemu, "-cpu", "max", "-cdrom", str(iso), "-drive",
                   f"file={qemu_value(original_disk)},format=raw,if=none,id=hd0,file.locking=off",
                   "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
                   "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
                   "-vga", "none", "-device", "virtio-gpu-pci", "-nic", "none",
                   "-audiodev", audio, *devices, "-serial", "stdio", "-monitor", "none",
                   "-display", "none", "-no-reboot", "-qmp",
                   f"unix:{monitor_path},server=on,wait=off"]
        result["command"] = command
        record(output / "command.json", command)
        with (output / "serial.log").open("wb") as serial, (output / "qemu.stderr").open("wb") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=serial, stderr=stderr)
            deadline = time.monotonic() + args.timeout
            sent = False
            while process.poll() is None and time.monotonic() < deadline:
                text = (output / "serial.log").read_bytes()
                # Wait for the actual ring-3 serial shell; LOGIT_BOOT_OK occurs
                # before its tty reader exists and can silently lose commands.
                # Kernel diagnostics can interleave with the banner's writes.
                # The executable-load record plus its prompt establish the
                # reader without requiring an unbroken product-name banner.
                shell_loaded = b"[exec] load /bin/sh:" in text
                if not sent and shell_loaded and b" $ " in text:
                    process.stdin.write(b"sndtest ramp 1000\n")
                    process.stdin.flush()
                    sent = True
                if sent and re.search(rb"SNDTEST_(RAMP_DONE|FAIL|NODEV)", text):
                    # snd_close(..., drain=1) completed; allow backend polling
                    # to consume its tail before the emulator is stopped.
                    time.sleep(2)
                    break
                time.sleep(0.1)
            result["command_sent"] = sent
            if process.poll() is None:
                # Use the normal monitor shutdown path and retain its reply.
                # QEMU 11 still leaves RIFF sizes zero in this configuration;
                # the PCM oracle handles only that exact header form below.
                with socket.socket(socket.AF_UNIX) as monitor:
                    monitor.settimeout(5)
                    monitor.connect(str(monitor_path))
                    stream = monitor.makefile("rwb")
                    transcript = [json.loads(stream.readline())]
                    for request in ({"execute": "qmp_capabilities"}, {"execute": "quit"}):
                        stream.write(json.dumps(request).encode() + b"\n")
                        stream.flush()
                        while True:
                            reply = json.loads(stream.readline())
                            transcript.append(reply)
                            if "return" in reply or "error" in reply:
                                break
                    record(output / "qmp.json", transcript)
            try:
                result["qemu_exit"] = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError("QEMU needed SIGKILL; WAV finalization is unproven")
        text = (output / "serial.log").read_text(errors="replace")
        if "LOGIT_BOOT_OK" not in text:
            result["errors"].append("kernel boot marker absent")
        if "LOGIT_PANIC" in text:
            result["errors"].append("guest kernel panic marker present")
        if "SNDTEST_RAMP_DONE frames=48000 mod=240 step=256 base=-30720" not in text:
            result["errors"].append("sndtest completion marker absent or unexpected")
        if re.search(r"SNDTEST_(FAIL|NODEV)", text):
            result["errors"].append("sndtest reported failure or missing card")
        info = re.search(r"SNDTEST_INFO driver=(\S+) codec=(.*?) rate=(\d+) ch=(\d+)", text)
        expected_driver = {"intel-hda": "hda", "AC97": "ac97", "ES1370": "es1370"}[args.device]
        if not info or info[1].lower() != expected_driver or int(info[3]) != rate or info[4] != "2":
            result["errors"].append("guest device identity/native format does not match attached card")
        result["guest_info"] = info[0] if info else None
        result["audio"] = inspect(wav, rate, allow_streaming_header=result["qemu_exit"] == 0)
        if result["qemu_exit"] != 0:
            result["errors"].append("QEMU did not exit normally")
        if result["audio"].get("metrics", {}).get("header_finalized") is False:
            # Preserve capture.wav as evidence. This derivative changes only
            # container lengths; it neither manufactures nor repairs samples.
            contents = bytearray(wav.read_bytes())
            struct.pack_into("<I", contents, 4, len(contents) - 8)
            struct.pack_into("<I", contents, 40, len(contents) - 44)
            playable = output / "playable.wav"
            playable.write_bytes(contents)
            result["header_note"] = "QEMU left zero lengths; playable.wav repairs only RIFF/data sizes"
            result["playable_sha256"] = digest(playable)
        if not result["audio"]["passed"]:
            result["errors"].extend(result["audio"]["errors"])
        if digest(original_disk) != disk_hash:
            result["errors"].append("input disk changed during snapshot run; evidence is not stable")
        result["wav_sha256"] = digest(wav)
        result["passed"] = not result["errors"]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result["errors"].append(str(error))
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        monitor_directory.cleanup()
        record(output / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--disk", type=Path)
    parser.add_argument("--device", choices=RATES)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        with tempfile.TemporaryDirectory(prefix="logitos-audio-oracle-") as temporary:
            result = self_test(Path(temporary))
        if args.output:
            args.output.mkdir(parents=True, exist_ok=True)
            record(args.output / "oracle.json", result)
        for case in result["cases"]:
            print(f"AUDIO_ORACLE {case['case']}: {'PASS' if case['control_ok'] else 'FAIL'}")
        return 0 if result["passed"] else 1
    if not all((args.iso, args.disk, args.device, args.output)):
        parser.error("guest execution requires --iso --disk --device --output")
    if not 1 <= args.timeout <= 600:
        parser.error("--timeout must be between 1 and 600 seconds")
    output = args.output.resolve()
    # A run never overwrites older evidence or accidentally accepts an old WAV.
    output.mkdir(parents=True, exist_ok=False)
    result = run(args, output)
    print(f"AUDIO_GUEST {args.device}: {'PASS' if result['passed'] else 'FAIL'} {output / 'result.json'}")
    for error in result["errors"]:
        print(f"  {error}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
