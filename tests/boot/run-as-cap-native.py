#!/usr/bin/env python3
"""Validate the prebuilt disk's native capcheck with actual kernel grants.

The input disk is copied under the normal disk ownership guard. Captured child
outputs and exit codes are read from that private disk after each run; serial
lines from separate children cannot accidentally satisfy a combined grep.
"""

import argparse
from datetime import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unit"))
sys.path.insert(0, str(ROOT / "tools"))
from as_capcheck_test import guest_assets, run_guest_capcheck
from as_examples import guest_command
from disk_guard import image_guard
from license_audit import _LogitFS


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    iso = args.iso.resolve()
    source_disk = args.disk.resolve()
    out = (args.out or ROOT / "build" / ("capcheck-prebuilt-" + datetime.now().strftime("%Y%m%d-%H%M%S"))).resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "name": "capcheck-prebuilt", "kernel_sha256": digest(iso)}
    guest = None
    try:
        # No test fixture is injected here. The shipped files and executable
        # must already be present, so this remains an installation gate.
        disk = out / "disk.img"
        with image_guard(source_disk):
            shutil.copyfile(source_disk, disk)
        report["input_disk_sha256"] = digest(disk)
        executable = guest_command(ROOT / "fsroot/as/examples/capcheck.as")
        assert executable == "/usr/as/bin/capcheck.aex", executable
        filesystem = _LogitFS(disk)
        try:
            for path in (executable, "/bin/native-capture", "/etc/logit.conf", "/usr/as/lib/sys.as"):
                inode = filesystem.lookup(path)
                if inode is None:
                    raise AssertionError(f"prebuilt disk is missing {path}")
                data = filesystem.read_file(inode)
                if path in (executable, "/bin/native-capture"):
                    assert data[:4] == b"AEX1", path
                report.setdefault("installed_sha256", {})[path] = hashlib.sha256(data).hexdigest()
        finally:
            filesystem.close()

        (out / "logit.iso").symlink_to(iso)
        _, grants = guest_assets(out)
        spec = importlib.util.spec_from_file_location("capcheck_guest", ROOT / "tests/boot/run-agent.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        guest = module.Guest(out, disk, out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        run_guest_capcheck(guest, executable, report, grants)
        report["passed"] = True
        print("PASS native capability gate: complete root/scoped/empty child outputs and real exits")
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if guest:
            guest.close()
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
