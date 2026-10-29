#!/usr/bin/env python3
"""Build both native Chat launchers and exercise the original visible UI gate.

The private image contains the real Chat app, fonts and CLI helpers, without
an AetherScript VM. The missing/malformed configuration gate opens the actual
window and submits through its prompt; it does not contact a model provider.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unit"))
sys.path.insert(0, str(ROOT / "tools"))
from as_gui_test import guest_assets
from license_audit import _LogitFS


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build, base, out = args.build.resolve(), args.base.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "kernel_sha256": digest(base / "logit.iso"),
              "compiler_sha256": digest(build / "asc"),
              "chat_sha256": digest(build / "ch.aex"), "modes": []}
    try:
        for mode in ("debug", "release"):
            directory = out / mode
            directory.mkdir()
            record = {"mode": mode, "passed": False}
            report["modes"].append(record)
            artifact = directory / "chlaunch.aex"
            command = [str(build / "asc"), "build", str(ROOT / "fsroot/as/examples/chlaunch.as"),
                       "--stdlib", str(ROOT / "fsroot/as/lib"), "--target", "logitos-x86_64",
                       "--json", "-o", str(artifact)]
            if mode == "debug":
                command.append("--debug")
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            (directory / "build.json").write_text(result.stdout)
            assert result.returncode == 0, result.stdout + result.stderr
            record.update(artifact_sha256=digest(artifact), snapshot=json.loads(result.stdout)["snapshot"])
            files = [f"{build}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo", "rm")]
            files += [f"{build}/ch.aex:/bin/ch.aex", f"{artifact}:/usr/as/bin/chlaunch.aex"]
            files.extend(f"{source}:{destination}" for source, destination in guest_assets())
            seed = directory / "seed.txt"
            seed.write_text("# Native Chat launcher test\n")
            files.append(f"{seed}:/etc/logit.conf")
            disk = directory / "disk.img"
            with (directory / "mkfs.log").open("w") as log:
                subprocess.run(["python3", "tools/mkfs.py", str(disk), *files], cwd=ROOT,
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            filesystem = _LogitFS(disk)
            try:
                assert filesystem.lookup("/bin/as") is None
                assert filesystem.lookup("/usr/as/lib/asc.la") is None
            finally:
                filesystem.close()
            record["vm_present"] = False
            with (directory / "refusal.log").open("w") as log:
                result = subprocess.run([
                    "python3", "tests/qmp/qmp_ch.py", "--iso", str(base / "logit.iso"),
                    "--disk", str(disk), "--only", "refusal", "--shots", str(directory / "shots"),
                ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=420)
            record["gate_exit"] = result.returncode
            assert result.returncode == 0, f"See {directory / 'refusal.log'}"
            record["passed"] = True
            print(f"PASS native Chat launcher {mode}: actual window, prompt and configuration refusal", flush=True)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
