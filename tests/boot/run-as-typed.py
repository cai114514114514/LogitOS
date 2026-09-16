#!/usr/bin/env python3
"""Run host-cross-compiled AetherScript artifacts inside a real LogitOS guest.

The private disk contains no AetherScript compiler or VM. A successful output
therefore comes from the packaged native executable and its linked runtime.
Both build modes and deliberately failing programs are required for acceptance.
Artifact hashes, exact command captures and the boot log stay in the run folder.
"""

import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/astyped"
sys.path.insert(0, str(ROOT / "tests/unit"))
from as_example_test import DEMO_OUTPUT, DEMO_SOURCE, EXAMPLES, GUEST_OUTPUTS
from as_process_test import ARGUMENTS, EXPECTED as PROCESS_OUTPUT
from as_constants_test import expected_output as constant_output
from as_capability_test import kernel_grants
from as_binary_test import binary_samples
from as_layout_test import guest_clock
from as_image_test import guest_images
from as_gui_test import guest_assets, run_guest_window
from as_shell_test import transcript as shell_transcript
from as_clock_test import guest_controls, check_guest_clock
from as_viewer_test import guest_viewer_assets, run_guest_viewer
from as_input_test import run_guest_input, check_stats
from as_capcheck_test import guest_assets as capcheck_assets, run_guest_capcheck
from as_barrier_test import run_guest_barriers


def load_guest_class():
    spec = importlib.util.spec_from_file_location("aether_guest", ROOT / "tests/boot/run-agent.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Guest


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(args, report):
    """Compile every fixture first; only finished AEX files enter the disk."""
    files = [f"{args.base}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo", "cat")]
    files.append(f"{args.build}/as-typed-capture.aex:/bin/native-capture")
    assets = guest_assets()
    files.extend(f"{source}:{destination}" for source, destination in assets)
    viewer_files, report["viewer_inputs"] = guest_viewer_assets(args.out)
    files.extend(f"{source}:{destination}" for source, destination in viewer_files)
    capability_files, report["capcheck_grants"] = capcheck_assets(args.out)
    files.extend(f"{source}:{destination}" for source, destination in capability_files)
    report["gui_assets"] = [
        {"path": destination, "sha256": digest(source)} for source, destination in assets
    ]
    state = args.out / "state.txt"
    state.write_text("native guest acceptance\n")
    files.append(f"{state}:/state/fixture")
    files.append(f"{state}:/state-outside/fixture")
    files.append(f"{state}:/docs/fixture")
    files.append(f"{state}:/dur/fixture")
    storage_input = args.out / "storage-readback.txt"
    storage_input.write_bytes(b"dddddddd")
    files.append(f"{storage_input}:/sp/a.bin")
    files.append(f"{EXAMPLES / 'hello.as'}:/usr/as/examples/hello.as")
    command_input = args.out / "command-input.txt"
    command_input.write_text("redirected\n")
    files.append(f"{command_input}:/state/command-output.txt")

    # A native child validates argv boundaries and exit status for the sys
    # library. It is deliberately not a VM script interpreted by a launcher.
    child = args.out / "sys-child.aex"
    child_source = FIXTURES / "sys-library/child.as"
    subprocess.run([str(args.build / "asc"), "build", str(child_source),
                    "--target", "logitos-x86_64", "-o", str(child)],
                   check=True, capture_output=True, text=True, timeout=60)
    files.append(f"{child}:/bin/sys-child")
    report["sys_child"] = {"sha256": digest(child), "source_sha256": digest(child_source)}
    command_child = args.out / "command-child.aex"
    command_child_source = FIXTURES / "command/child.as"
    subprocess.run([str(args.build / "asc"), "build", str(command_child_source),
                    "--target", "logitos-x86_64", "-o", str(command_child)],
                   check=True, capture_output=True, text=True, timeout=60)
    files.append(f"{command_child}:/bin/command-child")
    report["command_child"] = {"sha256": digest(command_child),
                               "source_sha256": digest(command_child_source)}

    cases = {
        "numeric": (0, "中文 native 30 4\n"),
        "modules": (0, "30\n"),
        "imports": (0, "native imports ok\n"),
        "failures": (1, "OverflowError"),
        "assertions": (1, "AssertionError"),
        "generics": (0, "6 3.75\n8 3\n120 泛型\n"),
        "generic-failure": (1, "OverflowError"),
        "exceptions": (0, "ZeroDivisionError 2\n42 4\ncaught 2\nValueError: 中文错误\nreraised 5\nelse\ndone\n"),
        "text": (0, "Aether 中文 second\ntext ok\n"),
        "uncaught": (1, "IOError: 读取失败"),
        "managed": (0, "managed stdlib ok\n"),
        "globals": (0, "module globals ok\n"),
        "numeric-lib": (0, "native numeric library ok\n"),
        "any": (0, "explicit Any ok\n"),
        "optional": (0, "native Optional ok\n"),
        "class": (0, "native classes ok\n"),
        "inheritance": (0, "native inheritance ok\n"),
        "buffer": (0, "native buffers ok\n"),
        "bytes": (0, "native Bytes ok\n"),
        "files": (0, "native files ok\n"),
        "ports": (0, "native scoped ports ok\n"),
        "ports-borrowed": (0, "borrowed stdout\nnative borrowed ports ok\n"),
        "ports-pipe": (0, "native pipe owners ok\n"),
        "ports-stats": (0, "native port statistics ok\n"),
        "commands": (0, "native commands ok\n"),
        "command-errors": (0, "redirected\nnative command errors ok\n"),
        "memory-text": (0, "native memory text ok\n"),
        "system-calls": (0, "native system calls ok\n"),
        "native-layout": (0, "native layouts ok\n"),
        "storage": (0, "native storage protocols ok\n"),
        "abi": (0, "native ABI argument guards ok\n"),
        "abi-guest": (0, "native ABI guest ok\n"),
        "sys-library": (0, "native sys argument checks ok\n"),
        "sys-library-guest": (0, "native sys guest ok\n"),
        "image-library": (0, "native image operations ok\n"),
        "image-errors": (0, "native image guest errors ok\n"),
        "gui-window": (0, "native GUI painted\nnative GUI key ok\n"),
        "example-guidemo": (0, "gui ok\n"),
        "studio-demo": (0, DEMO_OUTPUT),
        "temporary-roots": (0, "native temporary roots ok\n"),
        "decode": (0, "native UTF-8 decode ok\n"),
        "constants": (0, constant_output(args.out)),
        "capability": (0, "native capabilities ok\n"),
        "memory": (0, "native raw memory ok\n"),
        "pointer": (0, "native typed pointers ok\n"),
        "pointer-order": (0, "native pointer evaluation order ok\n"),
        "pointer-failure": (1, "ValueError"),
        "allocation": (0, "native manual allocation ok\n"),
        "allocation-failure": (1, "ValueError"),
        "region": (0, "native region ownership ok\n"),
        "region-failure": (1, "IndexError"),
        "region-borrow": (0, "native region borrows ok\n"),
        "region-borrow-failure": (1, "IndexError"),
        "aslex": (0, "native lexer library ok\n"),
        "fstring": (0, "native fstrings ok\n"),
        "range": (0, "native ranges and text iteration ok\n"),
        "comprehension": (0, "native comprehensions ok\n"),
        "unpack": (0, "native multiple assignment ok\n"),
        "unpack-module": (0, "native module unpack ok\n"),
        "compound": (0, "3\nnative compound assignment ok\n"),
        "process": (0, PROCESS_OUTPUT),
        "conversions": (0, "scalar conversions ok\n"),
        "dict": (0, "native dictionary ok\n"),
        "sets": (0, "native sets library ok\n"),
        "callable": (0, "native callable ok\n"),
        "closure": (0, "native closures ok\n"),
        "stats": (0, "native stats library ok\n"),
        "seq": (0, "native sequence library ok\n"),
        "dicts": (0, "native dictionary library ok\n"),
        "format": (0, "argument first\n[1] 2\ndynamic {'answer': 42} [true, false]\nnative formatting ok\n"),
        "test-lib": (0, "FAIL: recorded\nFAIL: list identity -- got [1] want [1]\n"
                        "FAIL: same text -- both x\nFAIL: different values\nFAIL: explicit\n"
                        "tests: 7 passed, 5 failed\ntests: 0 passed, 0 failed\nnative test library ok\n"),
    }
    entries = {name: FIXTURES / name / "main.as" for name in cases}
    entries["memory-text"] = FIXTURES / "native-system/memory.as"
    entries["pointer-order"] = FIXTURES / "pointer/order.as"
    entries["pointer-failure"] = FIXTURES / "pointer/failure.as"
    entries["allocation-failure"] = FIXTURES / "allocation/failure.as"
    entries["region-failure"] = FIXTURES / "region/failure.as"
    entries["region-borrow-failure"] = FIXTURES / "region-borrow/failure.as"
    entries["system-calls"] = FIXTURES / "native-system/guest.as"
    entries["abi-guest"] = FIXTURES / "abi/guest.as"
    entries["ports-borrowed"] = FIXTURES / "ports/borrowed.as"
    entries["ports-stats"] = FIXTURES / "ports/stats.as"
    entries["command-errors"] = FIXTURES / "command/errors.as"
    entries["ports-pipe"] = FIXTURES / "ports/pipe.as"
    entries["commands"] = FIXTURES / "command/main.as"
    entries["sys-library-guest"] = FIXTURES / "sys-library/guest.as"
    entries["image-errors"] = FIXTURES / "image-library/guest.as"
    entries["gui-window"] = FIXTURES / "gui-library/guest.as"
    entries["example-guidemo"] = EXAMPLES / "guidemo.as"
    entries["studio-demo"] = DEMO_SOURCE
    for name, data in {
        "empty-image": b"", "plain-image": b"ordinary text",
        "vector-image": b"<svg></svg>", "short-image": b"\x89PNG\r\n\x1a\n",
    }.items():
        sample = args.out / name
        sample.write_bytes(data)
        files.append(f"{sample}:/state/{name}")
    image_source, image_files, image_records = guest_images(args.out)
    files.extend(f"{source}:{destination}" for source, destination in image_files)
    report["image_inputs"] = image_records
    cases["image-pixels"] = (0, "native image guest pixels ok\n")
    entries["image-pixels"] = image_source
    entries["layout-clock"] = guest_clock(args.out)
    cases["layout-clock"] = (0, "native kernel layout ok\n")
    file_inputs = {}
    error_sources = {}
    # The original child must observe changed data and missing files; accepting
    # only its success marker would miss a hard-coded readback result.
    for suffix, data, expected in (
        ("short", b"ddd", "STORCHILD readback 3 bytes-ok 0\n"),
        ("changed", b"xdddddxd", "STORCHILD readback 8 bytes-ok 0\n"),
        ("missing", None, "STORCHILD readback -1 bytes-ok 0\n"),
    ):
        identity = "storage-child-" + suffix
        destination = "/state/" + identity
        checksum = None
        if data is not None:
            sample = args.out / identity
            sample.write_bytes(data)
            files.append(f"{sample}:{destination}")
            checksum = digest(sample)
        cases[identity] = (0, expected)
        entries[identity] = EXAMPLES / "storchild.as"
        file_inputs[identity] = (destination, checksum)
    file_inputs["example-storchild"] = ("/sp/a.bin", digest(storage_input))
    shell_script, shell_output, shell_interactive = shell_transcript(
        "/state", "/bin/command-child", guest=True)
    script = args.out / "ash-script.txt"
    script.write_text(shell_script)
    files.append(f"{script}:/state/ash-script.txt")
    for name, output in (("ash-script", shell_output), ("ash-interactive", shell_interactive)):
        cases[name] = (0, output)
        entries[name] = EXAMPLES / "ash.as"
    file_inputs["ash-script"] = ("/state/ash-script.txt", digest(script))
    for name, (data, status, output) in binary_samples().items():
        identity = "binary" if name == "valid" else "binary-" + name
        sample = args.out / (identity + ".bin")
        sample.write_bytes(data)
        destination = "/state/" + sample.name
        files.append(f"{sample}:{destination}")
        file_inputs[identity] = (destination, digest(sample))
        cases[identity] = (status, output)
        entries[identity] = FIXTURES / "binary/main.as"
        if status:
            error_sources[identity] = FIXTURES / "binary/reader.as"
    # Creation precedes reading in insertion order, so this pair verifies a
    # guest-produced file rather than only consuming host-seeded test data.
    for name, entry, output in (
        ("binary-create", "create.as", "wrote 4104 bytes\n"),
        ("binary-generated", "main.as", "header 3 4096\n"),
    ):
        cases[name] = (0, output)
        entries[name] = FIXTURES / "binary" / entry
        file_inputs[name] = ("/state/generated.bin", None)
    # Exercise the actual rewritten developer tool on a guest file. Expected
    # tokens come from the independent C lexer, not another run of aslex.as.
    lexer_source = FIXTURES / "numeric/main.as"
    lexer_oracle = subprocess.run([str(args.build / "asc"), "-lex", str(lexer_source)],
                                  capture_output=True, text=True, check=True, timeout=30)
    files.append(f"{lexer_source}:/state/lexer-input.as")
    cases["lexer-tool"] = (0, lexer_oracle.stdout)
    entries["lexer-tool"] = ROOT / "tests/unit/aslexdump.as"
    file_inputs["lexer-tool"] = ("/state/lexer-input.as", digest(lexer_source))
    grant_cases = {}
    grants = kernel_grants(args.out)
    for name, grant in grants.items():
        identity = "capability-" + name
        grant_cases[identity] = grant
        cases[identity] = (0, "native kernel grant ok\n")
        entries[identity] = FIXTURES / "capability-grant/main.as"
        file_identity = "files-" + name
        grant_cases[file_identity] = (grant[0], grant[1], "/state")
        cases[file_identity] = (0, "native files ok\n")
        entries[file_identity] = FIXTURES / "files/main.as"
        port_identity = "ports-grant-" + name
        grant_cases[port_identity] = (grant[0], grant[1], "/state")
        cases[port_identity] = (0, "native borrowed stdio\nnative port grants ok\n")
        entries[port_identity] = FIXTURES / "ports/grants.as"
        command_identity = "command-grant-" + name
        grant_cases[command_identity] = (grant[0], grant[1], "/state")
        cases[command_identity] = (0, "native command grants ok\n")
        entries[command_identity] = FIXTURES / "command/grants.as"
        system_identity = "system-grant-" + name
        grant_cases[system_identity] = grant
        cases[system_identity] = (0, "native system grants ok\n")
        entries[system_identity] = FIXTURES / "native-system/grants.as"
        pointer_identity = "pointer-grant-" + name
        grant_cases[pointer_identity] = grant
        cases[pointer_identity] = (0, "native pointer grants ok\n")
        entries[pointer_identity] = FIXTURES / "pointer/grants.as"
        allocation_identity = "allocation-grant-" + name
        grant_cases[allocation_identity] = grant
        cases[allocation_identity] = (0, "native allocation grants ok\n")
        entries[allocation_identity] = FIXTURES / "allocation/grants.as"
        image_identity = "image-grant-" + name
        grant_cases[image_identity] = (grant[0], grant[1], "/state")
        cases[image_identity] = (0, "native image grants ok\n")
        entries[image_identity] = FIXTURES / "image-library/grants.as"
    # Combine the authoritative kernel/language masks instead of copying either
    # numeric bit layout into this test. Only this grant can decode a real file.
    grant_cases["image-grant-allowed"] = (
        grants["fs"][0] | grants["raw"][0],
        grants["fs"][1] | grants["raw"][1], "/state",
    )
    cases["image-grant-allowed"] = (0, "native image grants ok\n")
    entries["image-grant-allowed"] = FIXTURES / "image-library/grants.as"
    for name, output in GUEST_OUTPUTS.items():
        identity = "example-" + name
        cases[identity] = (0, output)
        entries[identity] = EXAMPLES / (name + ".as")

    # These still wait on the real RTC, but report stopped/scaled monotonic
    # readings. The independent measurement oracle must reject both artifacts.
    for name, entry in guest_controls(args.out).items():
        cases[name] = (0, None)
        entries[name] = entry
    cases["example-asview"] = (0, None)
    entries["example-asview"] = EXAMPLES / "asview.as"
    for name in ("events", "evqstat", "capcheck", "barriers"):
        cases["example-" + name] = (0, None)
        entries["example-" + name] = EXAMPLES / (name + ".as")

    if args.case:
        unknown = set(args.case) - cases.keys()
        if unknown:
            raise ValueError(f"Unknown guest cases: {sorted(unknown)}")
        selected = set(args.case)
        # Interactive validation measures the original statistics tool before
        # and after input, so its native executable is a required companion.
        if "example-events" in selected:
            selected.add("example-evqstat")
        cases = {name: result for name, result in cases.items() if name in selected}
    for mode in ("debug", "release"):
        for name, (status, output) in cases.items():
            identity = f"{name}-{mode}"
            artifact = args.out / f"{identity}.aex"
            entry = entries[name]
            command = [str(args.build / "asc"), "build", str(entry), "--json",
                       "--target", "logitos-x86_64", "-o", str(artifact)]
            if mode == "debug":
                command.append("--debug")
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            (args.out / f"{identity}-build.json").write_text(result.stdout)
            if result.returncode:
                raise RuntimeError(f"Native build failed: {identity}: {result.stdout} {result.stderr}")
            built = json.loads(result.stdout)
            if not built["ok"] or artifact.read_bytes()[:4] != b"AEX1":
                raise AssertionError(f"Missing successful AEX build: {identity}")
            files.append(f"{artifact}:/bin/{identity}")
            if name in ("example-asview", "example-capcheck"):
                # The restricted-child case allows /usr/as only. Its program
                # must be there too; placing it under /bin tests exec denial,
                # not the viewer's ability to display a file-scope refusal.
                files.append(f"{artifact}:/usr/as/bin/{identity}.aex")
            error_source = str(error_sources.get(name, entry))
            if status and not any(source["path"] == error_source
                                  for source in built["snapshot"]["sources"]):
                raise AssertionError(f"Expected error source is outside snapshot: {identity}")
            report["programs"].append({
                "name": identity, "sha256": digest(artifact), "expected_status": status,
                "expected_output": output, "snapshot": built["snapshot"],
                "expected_error_source": error_source if status else None,
                "arguments": ([str(grant_cases[name][1]), grant_cases[name][2]]
                              if name in grant_cases else ARGUMENTS if name == "process"
                              else ["/state", "/bin/command-child"] if name == "commands"
                              else ["/state"] if name in ("files", "ports", "ports-borrowed", "ports-stats", "command-errors")
                              else [file_inputs[name][0]] if name in file_inputs else []),
                "input_sha256": file_inputs[name][1] if name in file_inputs else None,
                "grant": ([grant_cases[name][0], grant_cases[name][2]]
                          if name in grant_cases else None),
            })

    with (args.out / "mkfs.log").open("w") as log:
        subprocess.run(["python3", "tools/mkfs.py", str(args.out / "disk.img"), *files],
                       cwd=ROOT, stdout=log, check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", type=Path, default=ROOT / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", action="append", help="Run a named case in both modes (repeatable)")
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.base = args.base.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)

    report = {"passed": False, "complete": False, "programs": [], "checks": [],
              "selected_cases": args.case, "full_suite": not bool(args.case)}
    guest = None
    try:
        report["kernel_sha256"] = digest(args.base / "logit.iso")
        report["compiler_sha256"] = digest(args.build / "asc")
        prepare(args, report)
        guest = load_guest_class()(args.base, args.out / "disk.img", args.out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        for program in report["programs"]:
            name = program["name"]
            # The shell has no stderr redirect. The tiny guest launcher dup2s
            # its captured stdout onto stderr and execs this exact artifact.
            command = ["/bin/native-capture", "/bin/" + name, *program["arguments"]]
            if name.startswith("example-barriers-"):
                run_guest_barriers(guest, "/bin/" + name, program)
                report["checks"].append(name)
                print(f"PASS guest {name}: writeback device, real barrier counters and file readback", flush=True)
                continue
            if name.startswith("example-capcheck-"):
                run_guest_capcheck(guest, "/usr/as/bin/" + name + ".aex", program,
                                   report["capcheck_grants"])
                report["checks"].append(name)
                print(f"PASS guest {name}: real root/scoped/empty kernel grants", flush=True)
                continue
            if name.startswith("example-events-"):
                statistics = "/bin/" + name.replace("example-events-", "example-evqstat-")
                run_guest_input(guest, "/bin/" + name, program, args.out, statistics)
                report["checks"].append(name)
                print(f"PASS guest {name}: event fields, coordinates and queue counters", flush=True)
                continue
            if name.startswith("example-asview-"):
                status = run_guest_viewer(guest, "/usr/as/bin/" + name + ".aex", program,
                                          args.out, report["viewer_inputs"])
                program["exit_code"] = status
                assert status == 0, program
                report["checks"].append(name)
                print(f"PASS guest {name}: pixels, input, navigation and refusals", flush=True)
                continue
            if program["grant"] is not None:
                mask, prefix = program["grant"]
                command[1:1] = ["--caps", str(mask), prefix]
            if name.startswith(("gui-window-", "example-guidemo-")):
                status = run_guest_window(
                    guest, " ".join(shlex.quote(item) for item in command), program, args.out,
                )
                program["exit_code"] = status
                if status != 0:
                    raise AssertionError(f"{name}: GUI process exited {status}")
                report["checks"].append(name)
                print(f"PASS guest {name}: scanout, keyboard and exit {status}", flush=True)
                continue
            command_text = " ".join(shlex.quote(item) for item in command)
            if name.startswith("ash-interactive-"):
                # Feed the original native shell through a real guest pipe;
                # the parent shell never interprets this command transcript.
                command_text = "/bin/cat /state/ash-script.txt | " + command_text
            output = guest.capture(command_text, timeout=60)
            status = guest.last_capture_exit
            program.update(output=output, exit_code=status)
            if status != program["expected_status"]:
                raise AssertionError(f"{name}: exit {status}, output {output!r}")
            if status:
                # These are observed negative controls, not expected compiler
                # errors. The native guest must execute and report the failure.
                if program["expected_output"] not in output or "UNREACHABLE" in output:
                    raise AssertionError(f"{name}: missing propagated failure: {output!r}")
                # Errors can originate in an imported module. Check the exact
                # expected snapshot file and nonzero line/column, rather than
                # accepting a basename or forcing every failure into main.as.
                location = re.escape(program["expected_error_source"]) + r":[1-9]\d*:[1-9]\d*:"
                if not re.search(location, output):
                    raise AssertionError(f"{name}: missing original source location")
            elif name.startswith("example-evqstat-"):
                program["queue_counters"] = check_stats(output)
            elif program["expected_output"] is None:
                program["measurement"] = check_guest_clock(name, output)
            elif output != program["expected_output"]:
                raise AssertionError(f"{name}: wrong native output: {output!r}")
            report["checks"].append(name)
            print(f"PASS guest {name}: exit {status}", flush=True)
        report.update(passed=True, complete=True)
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if guest:
            guest.close()
        (args.out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
