#!/usr/bin/env python3
"""Run every Make-packaged A3 example on a disk with no AetherScript VM.

This validates the shipped artifact paths as well as program behavior. It does
not rebuild alternate test copies of the example sources. The native compiler
and remaining A2 fixtures are intentionally absent from the guest filesystem.
Long-lived fixtures receive explicit startup coverage; their dedicated stress
gates remain responsible for OOM, swap, and lifetime correctness.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
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


# These three programs deliberately stay alive. Observe their actual workload
# readiness last, then close this private guest; waiting for normal exit would
# take 20 minutes (elfspin), or forever (oomhog). This is packaged-startup
# coverage, not OOM selection, reclaim quality, or swap evidence. Their existing
# dedicated stress gates retain those stronger assertions.
STARTUP_PROBES = {
    "elfspin": ("packaged", rb"ELFSPIN-UP packaged\r?\n"),
    "oomsmall": ("4", rb"OOMSMALL-READY free frames now [0-9]+\r?\n"),
    "oomhog": ("64", rb"OOMHOG-RESIDENT 16384 pages\r?\n"),
}


def packaged_outputs():
    """Name every packaged executable independently of Make's selection.

    None selects a specific behavioral checker, never a skip. Keeping exact
    inventory equality catches new packaged sources with no execution oracle;
    deriving this dictionary from the native file list would defeat that check.
    """
    outputs = dict(GUEST_OUTPUTS, guidemo="gui ok\n", asview=None, events=None,
                   evqstat=None, capcheck=None, barriers=None, ash=None,
                   argvlimit=None, chlaunch="CHLAUNCH_OK\n", crashme=None,
                   durcheck=None, elfstat=None, mempress=None, mmaudit="0\n",
                   mmrep="0\n", oomrep="0\n", pcachecheck=None, pcachefill=None, ptracee=None,
                   ptracer="PTINTRUDE readable 0\n", setcheck="SETCHECK-SELFTEST-OK\n",
                   storgate=None, storprobe=None)
    for name in STARTUP_PROBES:
        outputs[name] = None
    return outputs


def check_packaged_output(name, output, expected):
    """Check effects and refusal results, not merely a program's final marker.

    Dynamic addresses/counters are checked by shape; fixed operation results
    remain literal. In particular the mm probes must print zero, so executing
    the wrong mmctl or returning audit inconsistencies cannot pass; oomrep
    returns the kill count, which is zero on this fresh boot.
    """
    if expected is not None:
        assert output == expected, (name, output)
        return
    if name == "monotonic":
        check_guest_clock(name, output)
        return
    assert not re.search(r"FAIL|CORRUPT|MISMATCH|IOError|PermissionError", output), (name, output)
    if name == "argvlimit":
        expected = ("argvlimit: 257 entries -> rc -7 E2BIG: true\n"
                    "argvlimit: 16 KiB string -> rc -7 E2BIG: true\n" +
                    " ".join(["k"] * 254 + ["ARGVLIMIT-LAST-256"]) + "\n"
                    "argvlimit: 256 entries exit 0\nargvlimit ok\n")
        assert output == expected, (name, output)
    elif name == "crashme":
        assert output == "CRASHME about to store to 3735928544\n", output
    elif name == "durcheck":
        assert output == "durcheck write /dur/packaged.bin 100 -> 100\n", output
    elif name == "elfstat":
        assert output == ("ELFSTAT-BEGIN packaged\nELFSTAT-AUDIT packaged 0\n"
                          "ELFSTAT-END packaged\n"), output
    elif name == "mempress":
        for line in ("MEMPRESS-ZEROED 4096", "MEMPRESS-TOUCHED 4096",
                     "MEMPRESS-ZERO-VERIFIED 0", "MEMPRESS-VERIFY-AT 0",
                     "MEMPRESS-OK 8192 pages verified", "MEMPRESS-UNMAPPED", "MEMPRESS-DONE"):
            assert line in output.splitlines(), (name, line, output)
    elif name == "pcachecheck":
        # SYS_MMAP_FILE in mmsys.c returns 0 for an invalid fd; only the
        # explicitly unsupported writable mapping has its named -1 result.
        for line in ("PC_BYTES ok", "PC_SECOND ok", "PC_WRITE_REFUSED -1",
                     "PC_BADFD 0", "pcachecheck: done"):
            assert line in output.splitlines(), (name, line, output)
    elif name == "pcachefill":
        mapped = re.search(r"^PCFILL-MAPPED (\d+) (\d+) (\d+) (\d+) (\d+)$", output, re.M)
        assert mapped and int(mapped[1]) > 0 and int(mapped[2]) > 0, output
        assert "PCFILL-AUDIT 0\nPCFILL-END\n" in output, output
    elif name == "ptracee":
        assert re.fullmatch(r"PTRACEE-UP\nPTRACEE-DONE [0-9]+\n", output), output
    elif name == "storgate":
        checks = [line for line in output.splitlines() if line.startswith("STORGATE-ok ")]
        assert checks == ["STORGATE-ok " + operation for operation in (
            "write-300 verified", "rewrite-120 verified", "ftruncate-grow-200 gap is zeros",
            "ftruncate-shrink-60 verified", "midfile-rewrite verified",
            "seek-past-end gap reads as zeros", "final-image written",
            "readonly ftruncate refused")], output
        assert output.endswith("STORGATE-RUN-DONE\n"), output
    elif name == "storprobe":
        # This is a capability report, not a claim that rename-over-existing
        # works. The first oracle draft expected replacement; the actual
        # 2026-11-01 serial run refused it (-1) and retained OLD, then renamed
        # that target to the fresh path. Keep that limitation visible beside
        # the correction instead of accepting either answer as success.
        for suffix in ("write 64 close 0 readback 64", "write 16 close 0 readback 80 byte63=A 1 byte64=b 1",
                       "lseek 4 write 3 close 0 readback 80 b3 65 b4 88 b6 90 b7 65",
                       "end 80 lseek 96 write 1 close 0 readback 97 hole-nonzero-bytes 0 last-byte 90",
                       "write 1 close 0 readback 97", "rc 0 close 0 readback 40",
                       "write 40 close 0 readback 40 byte0 99",
                       r'rc -1 rename-fresh rc 0 content-after Bytes("\x4f\x4c\x44")',
                       "write 8 fsync 0 close 0 readback 8", "child-exit 0"):
            assert any(line.endswith(suffix) for line in output.splitlines()), (suffix, output)
        assert "STORCHILD readback 8 bytes-ok 1\n" in output, output
        assert output.endswith("STORPROBE-DONE\n"), output
    else:
        raise AssertionError("No behavioral oracle for " + name)


def run_startup_probe(guest, name, executable):
    """Observe a live fixture's readiness without feeding the removed AS2 VM.

    Only 64 MiB is held by the hog on this 512 MiB guest. The innocent process
    remains at its free-frame wait; this does not exercise the known mm defect
    or claim that OOM recovery works. QEMU teardown owns all three processes.
    """
    argument, marker = STARTUP_PROBES[name]
    start = len(guest.log)
    guest.command(shlex.join([executable, argument]) + " &")
    # This 120-second host watchdog bounds a stuck fixture. It is not a
    # measured hardware startup deadline; completion is the guest marker.
    output = guest.wait(marker, 120, start).decode(errors="replace")
    assert "[fault] app exception" not in output, (name, output)
    return {"name": name, "coverage": "startup readiness; guest teardown stops workload",
            "output": output}


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
        outputs = packaged_outputs()
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
        files.append(f"{state}:/sg/fixture")
        storage_input = out / "storage-readback.txt"
        storage_input.write_bytes(b"dddddddd")
        files.append(f"{storage_input}:/sp/a.bin")
        for name in ("login", "sh", "echo", "cat", "true"):
            files.append(f"{base}/{name}.aex:/bin/{name}")
        files.append(f"{base}/ch.aex:/bin/ch.aex")
        files.append(f"{build}/as-typed-capture.aex:/bin/native-capture")
        files.append(f"{build}/as-input-capture.aex:/bin/input-capture")
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
            # Exercise ptracer's bounded unauthorized-read role here. Its
            # attach/register/lifetime contract stays in test-ptrace-os, just
            # as full durability and OOM tests remain separate from packaging.
            arguments = {"durcheck": " write /dur/packaged.bin tiny", "elfstat": " packaged",
                         "mempress": " small", "ptracer": " intrude", "setcheck": " selftest"}
            command += arguments.get(name, "")
            if name in STARTUP_PROBES:
                record = run_startup_probe(guest, name, native[name])
            elif name == "barriers":
                record = {"name": "example-barriers-packaged"}
                run_guest_barriers(guest, native[name], record)
            elif name == "capcheck":
                record = {"name": "example-capcheck-packaged"}
                run_guest_capcheck(guest, native[name], record, report["capcheck_grants"])
            elif name == "events":
                record = {"name": "example-events-packaged"}
                run_guest_input(guest, native[name], record, out, native["evqstat"],
                                capture="/bin/input-capture")
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
                # crashme deliberately faults at 0xdeadbee0. The shell's
                # signal status is 128 + SIGSEGV (11); success would mean the
                # unmapped store unexpectedly worked. All others must exit 0.
                status = 139 if name == "crashme" else 0
                assert guest.last_capture_exit == status, (name, guest.last_capture_exit, output)
                if name == "evqstat":
                    check_stats(output)
                else:
                    check_packaged_output(name, output, expected)
                if name == "durcheck":
                    verified = guest.capture("/bin/native-capture " + shlex.quote(native[name]) +
                                             " verify /dur/packaged.bin tiny")
                    assert guest.last_capture_exit == 0, verified
                    assert verified == "DURCHECK-OK /dur/packaged.bin 100\n", verified
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
