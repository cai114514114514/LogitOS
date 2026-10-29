#!/usr/bin/env python3
"""Native A3 durcheck on private persistent disks without a VM.

Each build mode writes five original fixture sizes, verifies exact disk bytes,
reboots, then verifies them before and after ordinary create/delete cycles.
This proves clean reboot persistence; it does not claim power-cut recovery.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unit"))
sys.path.insert(0, str(ROOT / "tools"))
from as_durability_test import SOURCE, SIZES, pattern
from as_gui_test import guest_assets
from license_audit import _LogitFS


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(guest, path):
    guest.qmp("stop", {})
    try:
        filesystem = _LogitFS(guest.disk)
        try:
            inode = filesystem.lookup(path)
            return filesystem.read_file(inode) if inode is not None else None
        finally:
            filesystem.close()
    finally:
        guest.qmp("cont", {})


def command(guest, record, arguments, output, status=0):
    line = shlex.join(["/bin/native-capture", "/usr/as/bin/durcheck.aex", *arguments])
    actual = guest.capture(line, timeout=180)
    result = {"arguments": arguments, "exit": guest.last_capture_exit, "output": actual}
    record.append(result)
    assert result["exit"] == status and actual == output, result


def exercise(Guest, build, base, out, mode, report):
    artifact = out / "durcheck.aex"
    arguments = [str(build / "asc"), "build", str(SOURCE), "--json", "--target",
                 "logitos-x86_64", "-o", str(artifact)]
    if mode == "debug":
        arguments.append("--debug")
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=120)
    (out / "build.json").write_text(result.stdout)
    assert result.returncode == 0, result
    report.update(artifact_sha256=digest(artifact), snapshot=json.loads(result.stdout)["snapshot"])
    (out / "seed").write_text("native durability fixture directory\n")
    damaged = bytearray(pattern(100))
    damaged[42] ^= 0xff
    (out / "damaged").write_bytes(damaged)
    (out / "short").write_bytes(pattern(99))
    files = [f"{build}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo")]
    files += [f"{build}/as-typed-capture.aex:/bin/native-capture",
              f"{artifact}:/usr/as/bin/durcheck.aex", f"{out}/seed:/dur/seed",
              f"{out}/seed:/state/fixture", f"{out}/damaged:/dur/damaged.bin",
              f"{out}/short:/dur/short.bin"]
    files.extend(f"{source}:{target}" for source, target in guest_assets())
    disk = out / "disk.img"
    with (out / "mkfs.log").open("w") as log:
        subprocess.run(["python3", "tools/mkfs.py", str(disk), *files], cwd=ROOT,
                       check=True, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    filesystem = _LogitFS(disk)
    try:
        for retired in ("/bin/as", "/usr/as/lib/asc.la", "/usr/as/lib/aslex.la"):
            assert filesystem.lookup(retired) is None, retired
    finally:
        filesystem.close()
    report["vm_present"] = False

    for phase in (1, 2):
        boot = out / f"boot-{phase}"
        boot.mkdir()
        record = {"phase": phase, "passed": False, "commands": [], "files": []}
        report["boots"].append(record)
        guest = Guest(base, disk, boot, "bios", "512M")
        try:
            guest.wait(b"LogitOS shell", 180)
            for name, size in SIZES.items():
                path = f"/dur/{name}.bin"
                if phase == 1:
                    command(guest, record["commands"], ["write", path, name],
                            f"durcheck write {path} {size} -> {size}\n")
                command(guest, record["commands"], ["verify", path, name],
                        f"DURCHECK-OK {path} {size}\n")
                content = read(guest, path)
                assert content == pattern(size), (mode, phase, path, "wrong guest disk bytes")
                record["files"].append({"path": path, "bytes": len(content),
                                        "sha256": hashlib.sha256(content).hexdigest()})
            command(guest, record["commands"], ["verify", "/dur/damaged.bin", "tiny"],
                    f"DURCHECK-FAIL /dur/damaged.bin first bad byte 42 got {damaged[42]} "
                    f"want {pattern(100)[42]}\n", 1)
            command(guest, record["commands"], ["verify", "/dur/short.bin", "tiny"],
                    "DURCHECK-FAIL /dur/short.bin length 99 expected 100\n", 1)
            command(guest, record["commands"], ["verify", "/dur/missing.bin", "tiny"],
                    "DURCHECK-FAIL /dur/missing.bin unreadable\n", 1)
            command(guest, record["commands"], ["write", "/missing/file", "tiny"],
                    "DURCHECK-FAIL /missing/file write\n", 1)
            command(guest, record["commands"], ["churn", "/dur"], "DURCHECK-CHURN-DONE\n")
            assert read(guest, "/dur/churn.tmp") is None
            for name, size in SIZES.items():
                path = f"/dur/{name}.bin"
                command(guest, record["commands"], ["verify", path, name],
                        f"DURCHECK-OK {path} {size}\n")
            record["passed"] = True
            print(f"PASS native durability {mode} boot {phase}: five sizes, exact bytes and failures",
                  flush=True)
        finally:
            guest.close()
    report["passed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", type=Path, default=ROOT / "build")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build, base, out = args.build.resolve(), args.base.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location("durability_guest", ROOT / "tests/boot/run-agent.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    report = {"passed": False, "kernel_sha256": digest(base / "logit.iso"),
              "compiler_sha256": digest(build / "asc"), "modes": []}
    try:
        for mode in ("debug", "release"):
            directory = out / mode
            directory.mkdir()
            record = {"mode": mode, "passed": False, "boots": []}
            report["modes"].append(record)
            exercise(module.Guest, build, base, directory, mode, record)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
