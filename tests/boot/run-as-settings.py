#!/usr/bin/env python3
"""Run native setcheck across four boots of a private persistent guest disk.

Every observation includes actual process output and exit code. Each mode has
a fresh disk without a VM; reboots use a new QEMU process on the same bytes.
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
from as_gui_test import guest_assets
from as_settings_test import SOURCE, GARBAGE
from license_audit import _LogitFS


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_file(guest, path):
    # Pause while reading committed metadata, so a kernel write cannot turn
    # the host observation into a torn filesystem read.
    guest.qmp("stop", {})
    try:
        filesystem = _LogitFS(guest.disk)
        try:
            inode = filesystem.lookup(path)
            assert inode is not None, path
            return filesystem.read_file(inode)
        finally:
            filesystem.close()
    finally:
        guest.qmp("cont", {})


def check(guest, records, arguments, output, status=0):
    command = shlex.join(["/bin/native-capture", "/usr/as/bin/setcheck.aex", *arguments])
    actual = guest.capture(command, timeout=60)
    record = {"arguments": arguments, "exit": guest.last_capture_exit, "output": actual}
    records.append(record)
    assert guest.last_capture_exit == status and actual == output, record


def boot_checks(guest, phase, records, directory):
    frame = "40 60 320 240 1 0 0 40 60 320 240"
    if phase == 1:
        check(guest, records, ["reset"], "SETCHECK-RESET\n")
        check(guest, records, ["set", "ui.dark", "1"], "SETCHECK-SET ui.dark = 1\n")
        check(guest, records, ["set", "ui.accent", "0x112233"],
              "SETCHECK-SET ui.accent = 0x112233\n")
        check(guest, records, ["frame", "clock", "40", "60", "320", "240"],
              "SETCHECK-SET win.clock.frame = " + frame + "\n")
        saved = read_file(guest, "/etc/settings.conf")
        (directory / "settings.conf").write_bytes(saved)
        assert b"ui.dark" in saved and b"0x112233" in saved, saved
    elif phase == 2:
        check(guest, records, ["get", "ui.dark"], "SETCHECK-VALUE ui.dark = 1\n")
        check(guest, records, ["check", "ui.accent", "0x112233"],
              "SETCHECK-OK ui.accent = 0x112233\n")
        check(guest, records, ["check", "win.clock.frame", frame],
              "SETCHECK-OK win.clock.frame = " + frame + "\n")
        check(guest, records, ["check", "ui.dark", "0"],
              "SETCHECK-BAD ui.dark want 0 got 1\n", status=1)
        check(guest, records, ["reload"], "SETCHECK-RELOAD 0\n")
        check(guest, records, ["check", "ui.dark", "1"], "SETCHECK-OK ui.dark = 1\n")
        old_size = len(read_file(guest, "/etc/settings.conf"))
        check(guest, records, ["truncate", "0"], f"SETCHECK-TRUNCATED 0 of {old_size}\n")
        assert read_file(guest, "/etc/settings.conf") == b""
    elif phase == 3:
        check(guest, records, ["check", "ui.dark", "0"], "SETCHECK-OK ui.dark = 0\n")
        check(guest, records, ["garbage"], f"SETCHECK-GARBAGE-WRITTEN {len(GARBAGE)}\n")
        assert read_file(guest, "/etc/settings.conf") == GARBAGE
    else:
        check(guest, records, ["check", "ui.dark", "0"], "SETCHECK-OK ui.dark = 0\n")
        check(guest, records, ["check", "net.ip", "10.0.2.15"],
              "SETCHECK-OK net.ip = 10.0.2.15\n")
        check(guest, records, ["selftest"], "SETCHECK-SELFTEST-OK\n")


def exercise(Guest, build, base, directory, mode, report):
    artifact = directory / "setcheck.aex"
    command = [str(build / "asc"), "build", str(SOURCE), "--json", "--target",
               "logitos-x86_64", "-o", str(artifact)]
    if mode == "debug":
        command.append("--debug")
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (directory / "build.json").write_text(result.stdout)
    assert result.returncode == 0, result.stdout + result.stderr
    report.update(artifact_sha256=digest(artifact), snapshot=json.loads(result.stdout)["snapshot"])
    files = [f"{build}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo")]
    files += [f"{build}/as-typed-capture.aex:/bin/native-capture",
              f"{artifact}:/usr/as/bin/setcheck.aex"]
    files.extend(f"{source}:{destination}" for source, destination in guest_assets())
    seed = directory / "seed.txt"
    seed.write_text("# A3 settings test: private disk\n")
    files.extend((f"{seed}:/state/fixture", f"{seed}:/etc/logit.conf"))
    disk = directory / "disk.img"
    with (directory / "mkfs.log").open("w") as log:
        subprocess.run(["python3", "tools/mkfs.py", str(disk), *files], cwd=ROOT,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
    filesystem = _LogitFS(disk)
    try:
        for retired in ("/bin/as", "/usr/as/lib/asc.la", "/usr/as/lib/aslex.la"):
            assert filesystem.lookup(retired) is None, retired
    finally:
        filesystem.close()
    report["vm_present"] = False

    for phase in range(1, 5):
        boot_dir = directory / f"boot-{phase}"
        boot_dir.mkdir()
        record = {"phase": phase, "commands": [], "passed": False}
        report["boots"].append(record)
        guest = Guest(base, disk, boot_dir, "bios", "512M")
        try:
            guest.wait(b"LogitOS shell", 180)
            boot_checks(guest, phase, record["commands"], boot_dir)
            record["passed"] = True
            print(f"PASS native settings {mode} boot {phase}: output, exit and disk bytes", flush=True)
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
    spec = importlib.util.spec_from_file_location("settings_guest", ROOT / "tests/boot/run-agent.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    report = {"passed": False, "kernel_sha256": digest(base / "logit.iso"),
              "compiler_sha256": digest(build / "asc"), "modes": []}
    try:
        for mode in ("debug", "release"):
            directory = out / mode
            directory.mkdir()
            current = {"mode": mode, "passed": False, "boots": []}
            report["modes"].append(current)
            exercise(module.Guest, build, base, directory, mode, current)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
