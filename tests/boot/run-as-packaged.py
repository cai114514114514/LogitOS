#!/usr/bin/env python3
"""Run every Make-packaged A3 example on a disk with no AetherScript VM.

This validates the shipped artifact paths as well as program behavior. It does
not rebuild alternate test copies of the example sources. The native compiler
and remaining A2 fixtures are intentionally absent from the guest filesystem.
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
from as_example_test import EXAMPLES, GUEST_OUTPUTS
from as_gui_test import guest_assets, run_guest_window
from as_shell_test import transcript
from as_examples import guest_command, source_version
from as_clock_test import check_guest_clock
from as_viewer_test import guest_viewer_assets, run_guest_viewer
from as_input_test import run_guest_input, check_stats
from as_capcheck_test import guest_assets as capcheck_assets, run_guest_capcheck
from as_barrier_test import run_guest_barriers


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
    report = {
        "passed": False,
        "kernel_sha256": digest(base / "logit.iso"),
        "compiler_sha256": digest(build / "asc"),
        "artifacts": {},
        "sources": {},
        "checks": [],
    }
    guest = None
    try:
        result = subprocess.run(
            ["make", "--no-print-directory", "-s", f"BUILD={build}", "as-native-example-files"],
            cwd=ROOT, capture_output=True, text=True, check=True, timeout=30)
        mapping = result.stdout.splitlines()
        native = {}
        files = []
        for entry in mapping:
            source, destination = entry.split(":", 1)
            artifact = Path(source)
            name = artifact.stem
            assert artifact.read_bytes()[:4] == b"AEX1", artifact
            assert name not in native, name
            native[name] = destination
            files.append(entry)
            report["artifacts"][destination] = digest(artifact)
            example = EXAMPLES / (name + ".as")
            assert source_version(example) == 3, example
            assert guest_command(example) == shlex.quote(destination), destination
            report["sources"][str(example.relative_to(ROOT))] = digest(example)
            files.append(f"{example}:/usr/as/examples/{example.name}")

        # This is the behavior oracle, independent of the Make selection. A
        # newly migrated example must add execution coverage before it passes.
        outputs = dict(GUEST_OUTPUTS, guidemo="gui ok\n", asview=None, events=None,
                       evqstat=None, capcheck=None, barriers=None)
        script, outputs["ash"], _ = transcript("/state", "/bin/command-child", guest=True)
        assert native.keys() == outputs.keys(), (sorted(native), sorted(outputs))
        script_file = out / "commands.txt"
        script_file.write_text(script)
        files.append(f"{script_file}:/state/commands.txt")
        state = out / "state.txt"
        state.write_text("native packaged examples\n")
        files.append(f"{state}:/state/fixture")
        # sysdemo writes under /docs, which the production filesystem provides.
        # Seed the same parent on this minimal acceptance disk.
        files.append(f"{state}:/docs/fixture")
        files.append(f"{state}:/dur/fixture")
        storage_input = out / "storage-readback.txt"
        storage_input.write_bytes(b"dddddddd")
        files.append(f"{storage_input}:/sp/a.bin")
        for name in ("login", "sh", "echo", "cat"):
            files.append(f"{base}/{name}.aex:/bin/{name}")
        files.append(f"{build}/as-typed-capture.aex:/bin/native-capture")
        files.extend(f"{source}:{destination}" for source, destination in guest_assets())
        viewer_files, report["viewer_inputs"] = guest_viewer_assets(out)
        files.extend(f"{source}:{destination}" for source, destination in viewer_files)
        capability_files, report["capcheck_grants"] = capcheck_assets(out)
        files.extend(f"{source}:{destination}" for source, destination in capability_files)

        child = out / "command-child.aex"
        subprocess.run([str(build / "asc"), "build",
                        str(ROOT / "tests/fixtures/astyped/command/child.as"),
                        "--target", "logitos-x86_64", "-o", str(child)],
                       check=True, capture_output=True, text=True, timeout=60)
        files.append(f"{child}:/bin/command-child")
        subprocess.run(["python3", "tools/mkfs.py", str(out / "disk.img"), *files],
                       cwd=ROOT, check=True, capture_output=True, text=True, timeout=60)

        spec = importlib.util.spec_from_file_location("packaged_guest", ROOT / "tests/boot/run-agent.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        guest = module.Guest(base, out / "disk.img", out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        for name, expected in outputs.items():
            command = "/bin/native-capture " + shlex.quote(native[name])
            if name == "ash":
                command += " /state/commands.txt"
            if name == "barriers":
                record = {"name": "example-barriers-packaged"}
                run_guest_barriers(guest, native[name], record)
            elif name == "capcheck":
                record = {"name": "example-capcheck-packaged"}
                run_guest_capcheck(guest, native[name], record, report["capcheck_grants"])
            elif name == "events":
                record = {"name": "example-events-packaged"}
                run_guest_input(guest, native[name], record, out, native["evqstat"])
            elif name == "asview":
                record = {"name": "example-asview-packaged"}
                status = run_guest_viewer(guest, native[name], record, out, report["viewer_inputs"])
                assert status == 0, (name, status)
                record["exit_code"] = status
            elif name == "guidemo":
                program = {"name": "example-guidemo-packaged", "expected_output": expected}
                status = run_guest_window(guest, command, program, out)
                assert status == 0, (name, status)
                record = {"name": name, "exit_code": status, "output": expected,
                          "gui_capture": program}
            else:
                output = guest.capture(command, timeout=60)
                assert guest.last_capture_exit == 0, (name, output)
                if name == "evqstat":
                    check_stats(output)
                elif expected is None:
                    check_guest_clock(name, output)
                else:
                    assert output == expected, (name, output)
                record = {"name": name, "exit_code": guest.last_capture_exit, "output": output}
            report["checks"].append(record)
            print(f"PASS packaged A3 example: {name}", flush=True)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if guest:
            guest.close()
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
