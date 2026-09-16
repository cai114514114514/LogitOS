#!/usr/bin/env python3
"""Check the shipped A3 launcher without any retiring compiler caches."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


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
    source = out / "native.as"
    source.write_text('''# aether: 3.0
def main() -> None:
    with owner = region(1):
        with moved = owner.move():
            with view = moved.borrow_mut(0, 1):
                view[0] = 7
            assert moved[0] == 7 and len(moved) == 1
    unsafe:
        storage = alloc(4)
        pointer: Ptr[i32] = i32ptr(addr(storage))
        pointer[0] = 7
        assert pointer[0] == 7
        dealloc(storage)
    command = run("echo", "native CLI guest") |> run("cat")
    with reader, writer = pipe():
        assert port_stats()["open"] == 2
        writer.write(command.out())
        writer.close()
        output = reader.readall()
        with console = port(1):
            console.write(output)
    assert port_stats()["open"] == 0
''')
    report = {"passed": False, "checks": [], "compiler_sha256": digest(build / "as.aex"),
              "kernel_sha256": digest(base / "logit.iso"), "source_sha256": digest(source)}
    invalid = out / "moved.as"
    invalid.write_text('''# aether: 3.0
def main() -> None:
    with owner = region(1):
        with moved = owner.move():
            pass
        owner[0] = 1
''')
    report["moved_source_sha256"] = digest(invalid)
    conflict = out / "borrow-conflict.as"
    conflict.write_text('''# aether: 3.0
def main() -> None:
    with owner = region(1):
        with view = owner.borrow(0, 1):
            owner[0] = 1
''')
    report["borrow_source_sha256"] = digest(conflict)
    guest = None
    try:
        artifact = out / "program.aex"
        subprocess.run([str(build / "asc"), "build", str(source), "--target", "logitos-x86_64",
                        "-o", str(artifact)], capture_output=True, text=True, check=True, timeout=60)
        files = [f"{base}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo", "cat")]
        files.extend([f"{build}/as.aex:/bin/as", f"{build}/as-typed-capture.aex:/bin/native-capture",
                      f"{source}:/state/native.as", f"{invalid}:/state/moved.as",
                      f"{conflict}:/state/borrow-conflict.as",
                      f"{artifact}:/bin/native-program"])
        # No asc.la/aslex.la, VM library cache or LLVM compiler is packaged.
        # A3 checking must work; local native build must name its missing host
        # connection. Neither behavior may silently invoke the old compiler.
        subprocess.run(["python3", "tools/mkfs.py", str(out / "disk.img"), *files],
                       cwd=ROOT, capture_output=True, text=True, check=True, timeout=60)
        spec = importlib.util.spec_from_file_location("native_cli_guest", ROOT / "tests/boot/run-agent.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        guest = module.Guest(base, out / "disk.img", out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        output = guest.capture("/bin/native-capture /bin/as check --json /state/native.as")
        checked = json.loads(output)
        assert guest.last_capture_exit == 0 and checked["ok"] and checked["language"] == 3, output
        assert checked["source_bytes"] == len(source.read_bytes()), checked
        report["checks"].append({"name": "A3 check without A2 caches", "result": checked})
        output = guest.capture("/bin/native-capture /bin/as check --json /state/moved.as")
        rejected = json.loads(output)
        assert guest.last_capture_exit == 1 and not rejected["ok"], rejected
        assert len(rejected["diagnostics"]) == 1, rejected
        diagnostic = rejected["diagnostics"][0]
        assert diagnostic["code"] == "AS3402" and diagnostic["line"] == 6, diagnostic
        assert diagnostic["source_checksum"] == rejected["source_checksum"], rejected
        report["checks"].append({"name": "Region move rejected by guest compiler", "result": rejected})
        output = guest.capture("/bin/native-capture /bin/as check --json /state/borrow-conflict.as")
        rejected = json.loads(output)
        assert guest.last_capture_exit == 1 and not rejected["ok"], rejected
        assert len(rejected["diagnostics"]) == 1, rejected
        diagnostic = rejected["diagnostics"][0]
        assert diagnostic["code"] == "AS3403" and diagnostic["line"] == 5, diagnostic
        assert len(diagnostic["related"]) == 1 and diagnostic["related"][0]["line"] == 4, diagnostic
        assert diagnostic["related"][0]["source_checksum"] == rejected["source_checksum"], diagnostic
        report["checks"].append({"name": "Borrow conflict and origin in guest compiler", "result": rejected})
        output = guest.capture("/bin/native-capture /bin/as /state/native.as")
        assert guest.last_capture_exit == 1 and "AS3501" in output, output
        assert "Host LLVM build connection is required" in output, output
        assert "cannot run in the A2 VM" not in output, output
        report["checks"].append({"name": "shorthand names missing host connection", "output": output})
        output = guest.capture("/bin/native-capture /bin/native-program")
        assert guest.last_capture_exit == 0 and output == "native CLI guest\n", output
        report["checks"].append({"name": "same source executes as native AEX", "output": output,
                                 "sha256": digest(artifact)})
        report["passed"] = True
        print("PASS native CLI guest: A3 check, explicit missing-host diagnostic and native artifact")
    finally:
        if guest:
            guest.close()
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
