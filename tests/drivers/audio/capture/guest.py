#!/usr/bin/env python3
"""Inject synthetic PCM through QEMU's real D-Bus input backend into guest ADCs."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time

from oracle import inspect, self_test
from profiles import NATIVE_RATES, device_arguments

ROOT = Path(__file__).resolve().parents[4]
RATES = NATIVE_RATES


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def source_pcm(path, frames):
    # A long deterministic sequence with independent channels; multiples of 15
    # keep AC97's observed QEMU gain 136/255 exactly representable in s16.
    state = 0x8A31B527
    values = array.array("h")
    for _ in range(frames * 2):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        values.append(((state % 1601) - 800) * 15)
    import sys
    if sys.byteorder != "little":
        values.byteswap()
    path.write_bytes(values.tobytes())


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile("rb")
        self.transcript = [json.loads(self.stream.readline())]
        self.call("qmp_capabilities")

    def call(self, execute, arguments=None, fd=None):
        request = {"execute": execute}
        if arguments is not None:
            request["arguments"] = arguments
        data = json.dumps(request).encode() + b"\n"
        if fd is None:
            self.socket.sendall(data)
        else:
            self.socket.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                          array.array("i", [fd]))])
        while True:
            response = json.loads(self.stream.readline())
            self.transcript.append(response)
            if "error" in response:
                raise RuntimeError(str(response))
            if "return" in response:
                return response["return"]


def wait_for(predicate, processes, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if any(process.poll() is not None for process in processes):
            raise RuntimeError("emulator or synthetic backend exited early")
        time.sleep(0.1)
    raise RuntimeError("guest/backend deadline exceeded")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--device", choices=RATES, required=True)
    parser.add_argument("--output-device", choices=("intel-hda",),
                        help="use a separate, output-only HDA card before the input card")
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {"passed": False, "device": args.device,
              "output_device": args.output_device or args.device, "errors": []}
    qemu = backend = monitor = None
    handles = []
    original_disk_hash = None
    try:
        controls = self_test()
        save(output / "oracle-controls.json", controls)
        if not controls["passed"]:
            raise RuntimeError("ADC oracle positive/negative prerequisites failed")
        rate = RATES[args.device]
        output_rate = RATES[args.output_device or args.device]
        card = device_arguments(args.device, args.output_device)
        result["native_rates"] = {"capture": rate, "playback": output_rate}
        iso, disk = output / "input.iso", output / "disk.img"
        shutil.copyfile(args.iso, iso)
        original_disk_hash = sha(args.disk)
        shutil.copyfile(args.disk, disk)
        if sha(args.disk) != original_disk_hash or sha(disk) != original_disk_hash:
            raise RuntimeError("source disk changed while copying")
        result["inputs"] = {"iso_sha256": sha(iso), "disk_sha256": sha(disk),
                            "backend_sha256": sha(args.backend),
                            "original_disk_sha256": original_disk_hash}
        source_pcm(output / "source.pcm", rate * 20)
        result["source_sha256"] = sha(output / "source.pcm")
        with tempfile.TemporaryDirectory(prefix="adc-qmp-") as temporary:
            qmp_path = Path(temporary) / "qmp.sock"
            audio = (f"dbus,id=snd0,in.frequency={rate},out.frequency={output_rate},"
                     "in.channels=2,out.channels=2,in.format=s16,out.format=s16")
            command = ["qemu-system-x86_64", "-S", "-cpu", "max", "-m", "512M",
                       "-smp", "4", "-accel", "tcg,thread=multi", "-boot", "d",
                       "-cdrom", str(iso), "-drive", f"file={str(disk).replace(',', ',,')},format=raw,if=none,id=hd0",
                       "-device", "virtio-blk-pci,drive=hd0", "-vga", "none",
                       "-device", "virtio-gpu-pci", "-nic", "none", "-audiodev", audio,
                       *card, "-display", "dbus,p2p=on,audiodev=snd0", "-serial", "stdio", "-monitor", "none",
                       "-qmp", f"unix:{qmp_path},server=on,wait=off", "-no-reboot"]
            result["qemu_version"] = subprocess.check_output(
                [command[0], "--version"], text=True).splitlines()[0]
            save(output / "command.json", command)
            serial = (output / "serial.log").open("wb")
            stderr = (output / "qemu.stderr").open("wb")
            handles.extend((serial, stderr))
            qemu = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=serial, stderr=stderr)
            wait_for(qmp_path.exists, [qemu], 10)
            monitor = Monitor(qmp_path)
            host, remote = socket.socketpair()
            monitor.call("getfd", {"fdname": "audio-peer"}, remote.fileno())
            monitor.call("add_client", {"protocol": "@dbus-display", "fdname": "audio-peer"})
            remote.close()
            backend_log = (output / "backend.log").open("wb")
            backend_error = (output / "backend.stderr").open("wb")
            handles.extend((backend_log, backend_error))
            backend = subprocess.Popen([str(args.backend.resolve()), str(host.fileno()), str(rate),
                                        str(output / "source.pcm"), str(output / "injected.pcm"),
                                        str(output / "playback.pcm"), str(output_rate)], pass_fds=[host.fileno()],
                                       stdout=backend_log, stderr=backend_error)
            host.close()
            wait_for(lambda: "BACKEND_READY" in (output / "backend.log").read_text(), [qemu, backend], 15)
            monitor.call("cont")
            text = lambda: (output / "serial.log").read_text(errors="replace")
            wait_for(lambda: "[exec] load /bin/sh:" in text() and " $ " in text(), [qemu, backend])
            for round_number in (1, 2):
                if round_number == 2:
                    qemu.stdin.write(b"sndtest ramp 3000 &\n")
                    qemu.stdin.flush()
                qemu.stdin.write(f"rec 1 /capture{round_number}.wav\n".encode())
                qemu.stdin.flush()
                wait_for(lambda: f"path=/capture{round_number}.wav" in text() or "REC_FAIL" in text(),
                         [qemu, backend])
                if "REC_FAIL" in text():
                    raise RuntimeError("rec reported failure")
            wait_for(lambda: "SNDTEST_RAMP_DONE" in text(), [qemu, backend])
            qemu.stdin.write(b"sync\n")
            qemu.stdin.flush()
            time.sleep(2)
            monitor.call("quit")
            result["qemu_exit"] = qemu.wait(timeout=10)
            result["backend_exit"] = backend.wait(timeout=10)
            save(output / "qmp.json", monitor.transcript)
        for round_number in (1, 2):
            subprocess.run(["python3", str(ROOT / "tests/boot/lfs_extract.py"), str(disk),
                            f"/capture{round_number}.wav", str(output / f"capture{round_number}.wav")], check=True)
        guest_log = (output / "serial.log").read_text(errors="replace")
        if "LOGIT_BOOT_OK" not in guest_log or "LOGIT_PANIC" in guest_log:
            result["errors"].append("guest did not boot cleanly")
        if args.output_device and "REC_INFO driver=hda" not in guest_log:
            result["errors"].append("mixed fixture did not retain HDA as its playback device")
        for round_number in (1, 2):
            expected = f"REC_DONE frames={rate} bytes={rate * 4} path=/capture{round_number}.wav"
            if expected not in guest_log:
                result["errors"].append(f"round {round_number}: rec completion fields differ")
        # rec prints fields through separate writes, so concurrent kernel
        # diagnostics can split even a number. The two WAV headers establish
        # native rate/stereo/s16 and the backend's enable intervals establish
        # two actual opens; inspect() below requires both. Keep intact text
        # for diagnostics, without treating interleaved serial as PCM failure.
        result["rec_open_lines"] = re.findall(r"(?m)^REC_OPEN[^\r\n]*", guest_log)
        if "SNDTEST_RAMP_DONE frames=144000 mod=240 step=256 base=-30720" not in guest_log:
            result["errors"].append("duplex playback completion fields differ")
        if re.search(r"REC_(FAIL|SHORT)|SNDTEST_(FAIL|NODEV)", guest_log):
            result["errors"].append("guest reported recording/playback failure")
        result["capture_extracted"] = True
        result["pcm"] = inspect(output, rate, 136 if args.device == "AC97" else None,
                                output_rate)
        result["errors"].extend(result["pcm"]["errors"])
        if result["qemu_exit"] != 0 or result["backend_exit"] != 0:
            result["errors"].append("QEMU or synthetic backend did not exit normally")
        if re.search(r"BACKEND_FAIL|AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:",
                     (output / "backend.stderr").read_text()):
            result["errors"].append("synthetic backend reported failure")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        result["errors"].append(str(error) or "capture run interrupted")
    finally:
        for process in (qemu, backend):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for handle in handles:
            handle.close()
        # No writer remains alive when observations are hashed. This includes
        # failed runs, whose incomplete artifacts are still useful evidence.
        result["artifacts"] = {name: sha(output / name) for name in
            ("source.pcm", "injected.pcm", "playback.pcm", "capture1.wav", "capture2.wav",
             "backend.log", "backend.stderr", "serial.log", "qemu.stderr")
            if (output / name).exists()}
        result["harness"] = {str(path): sha(path) for path in
            (Path(__file__).resolve(), Path(__file__).with_name("oracle.py"),
             Path(__file__).with_name("backend.c"), Path(__file__).with_name("profiles.py"))}
        if original_disk_hash is not None:
            try:
                result["original_disk_after_sha256"] = sha(args.disk)
                if result["original_disk_after_sha256"] != original_disk_hash:
                    result["errors"].append("original input disk changed during the run")
            except OSError as error:
                result["errors"].append(f"original disk could not be rechecked: {error}")
        result["passed"] = not result["errors"]
        save(output / "result.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
