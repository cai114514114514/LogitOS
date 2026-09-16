#!/usr/bin/env python3
"""Run the actual Region runtime and instrumented ownership probes in LogitOS.

This verifies the C backend, not source-language lifetime rules. The report
keeps that boundary explicit until the compiler's Region lowering is ready.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unit"))
from as_region_test import EXPECTED, SOURCE, UNITS


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", type=Path, default=ROOT / "build")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build, base, out = args.build.resolve(), args.base.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = ROOT / "c/apps/as/runtime"
    report = {
        "passed": False,
        "scope": "native Region runtime; source-language ownership analysis is not covered",
        "kernel_sha256": digest(base / "logit.iso"),
        "sources": {str(path.relative_to(ROOT)): digest(path)
                    for path in [SOURCE, *(runtime / name for name in UNITS),
                                 runtime / "region.h", runtime / "region_internal.h"]},
        "checks": [],
    }
    guest = None
    try:
        # Guest.capture writes under /state and obtains status/markers through
        # the real echo program. Both are part of the measurement apparatus.
        files = [f"{base}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo")]
        seed = out / "state.txt"
        seed.write_text("Region runtime acceptance\n")
        files.append(f"{seed}:/state/fixture")
        files.append(f"{build}/as-typed-capture.aex:/bin/native-capture")
        for name in ("runtime", "tracked"):
            files.append(f"{build}/as-region-{name}.aex:/bin/region-{name}")
        subprocess.run(["python3", "tools/mkfs.py", str(out / "disk.img"), *files],
                       cwd=ROOT, capture_output=True, text=True, check=True, timeout=60)
        spec = importlib.util.spec_from_file_location("region_guest", ROOT / "tests/boot/run-agent.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        guest = module.Guest(base, out / "disk.img", out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        for name, tracked in (("runtime", False), ("tracked", True)):
            command = f"/bin/native-capture /bin/region-{name}"
            if tracked:
                command += " --tracked"
            output = guest.capture(command, timeout=60)
            result = {"name": name, "exit_code": guest.last_capture_exit, "output": output,
                      "sha256": digest(build / f"as-region-{name}.aex")}
            report["checks"].append(result)
            assert result["exit_code"] == 0 and output == EXPECTED[tracked], result
            print(f"PASS guest Region {name}: {output.strip()}", flush=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if guest:
            guest.close()
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
