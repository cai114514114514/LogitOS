#!/usr/bin/env python3
"""Hard cutover gate: version relabeling and deletion cannot satisfy migration.

The frozen API inventory detects dropped public names and examples. Successful
native checking/building is necessary but not sufficient for behavior parity;
the independent native/guest suites remain prerequisites of the Make target.
This gate intentionally remains red while the old engine or sources remain.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "tests/fixtures/astyped/migration-api.json"
BUILTINS = ROOT / "tests/fixtures/astyped/migration-builtins.json"
LIBRARY = ROOT / "fsroot/as/lib"
RETIRED_ENGINE = (
    "legacy/vm.c", "legacy/compiler.c", "legacy/bytecode.c", "legacy/object.c",
    "legacy/value.c", "legacy/builtins.c", "legacy/memory.c", "legacy/output.c",
    "legacy/ports.c", "legacy/bootstrap.as",
)

# These lowering probes complement the executed native suites. They do not
# claim behavior parity by themselves. A migrated runtime API needs a valid
# probe here AND real positive/negative execution coverage in the native gate.
# Region was deliberately unlisted when only its constructor/move existed.
# Its probe now exercises checked views, snapshots and transfer as well. This
# still proves native lowering only; source/guest gates prove those behaviors,
# and the remaining library/tool/selfhost blockers continue to prevent cutover.
BUILTIN_PROBES = {
    "addr": "unsafe:\n        addr(buffer(1))",
    "alloc": "unsafe:\n        memory = alloc(8)\n        dealloc(memory)",
    "args": "args()",
    "buffer": "buffer(2)",
    "caps": "caps()",
    "chr": "chr(65)",
    "dealloc": "unsafe:\n        memory = alloc(8)\n        dealloc(memory)",
    "f64bits": "f64bits(1.5)",
    "file_read": 'file_read("data.bin")',
    "file_write": 'file_write("data.bin", Bytes("data"))',
    "gc": "gc()",
    "gc_stats": "gc_stats()",
    "i8ptr": "unsafe:\n        i8ptr(0)",
    "i16ptr": "unsafe:\n        i16ptr(0)",
    "i32ptr": "unsafe:\n        i32ptr(0)",
    "i64ptr": "unsafe:\n        i64ptr(0)",
    "len": "len([1, 2])",
    "layout": "record = Record()\n    record.value = 42\n    assert record.value == 42",
    "mem2str": 'unsafe:\n        mem2str(Bytes("text"), 4)',
    "mem2cstr": 'unsafe:\n        mem2cstr(buffer(1))',
    "ord": 'ord("a")',
    "open": 'with file = open("data.bin"):\n        file.readall()',
    "port": 'with console = port(1):\n        console.write("native")',
    "port_stats": 'assert port_stats()["open"] >= 0',
    "pipe": 'with reader, writer = pipe():\n        writer.close()\n        reader.readall()',
    "parse_float": 'parse_float("1.5")',
    "parse_int": 'parse_int("42")',
    "peek8": "unsafe:\n        peek8(addr(buffer(1)))",
    "peek16": "unsafe:\n        peek16(addr(buffer(2)))",
    "peek32": "unsafe:\n        peek32(addr(buffer(4)))",
    "peek64": "unsafe:\n        peek64(addr(buffer(8)))",
    "poke8": "unsafe:\n        poke8(addr(buffer(1)), 1)",
    "poke16": "unsafe:\n        poke16(addr(buffer(2)), 1)",
    "poke32": "unsafe:\n        poke32(addr(buffer(4)), 1)",
    "poke64": "unsafe:\n        poke64(addr(buffer(8)), 1)",
    "print": 'print("native")',
    "range": "range(3)",
    "region": "with owner = region(2):\n        with writer = owner.borrow_mut(0, 2):\n            writer[0] = 42\n        with reader = owner.borrow(0, 2):\n            assert Bytes(reader)[0] == 42\n        with moved = owner.move():\n            assert moved[0] == 42",
    "run": 'command = run("echo", "native")\n    with process = command:\n        process.wait()',
    "str": "str(42)",
    "syscall": "unsafe:\n        syscall(SYS_GETPID)",
    "wrapping_add": "wrapping_add(1, 2)",
    "wrapping_mul": "wrapping_mul(2, 3)",
    "wrapping_shl": "wrapping_shl(1, 3)",
    "wrapping_sub": "wrapping_sub(3, 1)",
}

# A3 layout metadata is a module declaration, not an executable factory call.
# Keep its native probe faithful to that migration boundary.
BUILTIN_DECLARATIONS = {
    "layout": 'Record = layout("record", 4, [["value", 0, 4, "i"]])\n',
}


def check_builtins(compiler, report, work):
    baseline = json.loads(BUILTINS.read_text())
    report["builtins"] = []
    for name in baseline["functions"]:
        probe = BUILTIN_PROBES.get(name)
        lowered = False
        if probe:
            source = work / ("builtin-" + name + ".as")
            source.write_text("# aether: 3.0\n" + BUILTIN_DECLARATIONS.get(name, "") +
                              "def main() -> None:\n    " + probe + "\n")
            result = subprocess.run(
                [str(compiler), "build", str(source), "--emit-llvm", "-o", str(work / "builtin.ll")],
                capture_output=True, text=True, timeout=90)
            lowered = result.returncode == 0
        report["builtins"].append({"name": name, "lowered": lowered})
        if not lowered:
            report["blockers"].append(f"Builtin lacks successful native migration probe: {name}")

    # Checking all constants in a single module keeps the audit cheap. Their
    # numeric values and platform effects still require ABI/runtime tests.
    source = work / "constants.as"
    statements = [f"    constant_{index}: i64 = {name}"
                  for index, name in enumerate(baseline["constants"])]
    source.write_text("# aether: 3.0\ndef main() -> None:\n" + "\n".join(statements) + "\n")
    result = subprocess.run([str(compiler), "check", str(source), "--json"],
                            capture_output=True, text=True, timeout=90)
    report["constants"] = {"count": len(statements), "checked": result.returncode == 0}
    if result.returncode:
        report["blockers"].append("Registered system constants lack a successful native check")


def declarations(source):
    functions = set(re.findall(r"^def ([A-Za-z][A-Za-z_0-9]*)[\[(]", source, re.MULTILINE))
    values = set(re.findall(r"^([A-Za-z][A-Za-z_0-9]*)\s*(?::[^=\n]+)?=", source, re.MULTILINE))
    classes = {}
    current = None
    for line in source.splitlines():
        match = re.match(r"^class (\w+)", line)
        if match:
            current = match.group(1)
            classes[current] = set()
        elif line and not line[0].isspace() and not line.startswith("#"):
            current = None
        elif current:
            method = re.match(r"    def (\w+)[\[(]", line)
            if method:
                classes[current].add(method.group(1))
    return functions, values, classes


def inventory(compiler):
    baseline = json.loads(BASELINE.read_text())
    report = {"passed": False, "target": "3.0", "blockers": [], "libraries": [], "examples": []}
    blockers = report["blockers"]
    for name in RETIRED_ENGINE:
        if (ROOT / "c/apps/as" / name).exists():
            blockers.append(f"Retired engine remains: c/apps/as/{name}")
    if (ROOT / "fsroot/as/compat2").exists():
        blockers.append("Retired compatibility directory remains: fsroot/as/compat2")

    listed = subprocess.check_output(["rg", "--files", "-g", "*.as"], cwd=ROOT, text=True)
    for relative in listed.splitlines():
        source = (ROOT / relative).read_text()
        if not source.splitlines() or source.splitlines()[0] not in ("# aether: 3", "# aether: 3.0"):
            blockers.append(f"Source has not migrated to A3: {relative}")

    for module in baseline["libraries"]:
        path = ROOT / module["path"]
        if not path.is_file():
            blockers.append(f"Standard library module disappeared: {module['path']}")
            continue
        source = path.read_text()
        functions, values, classes = declarations(source)
        for name in set(module["functions"]) - functions:
            blockers.append(f"Missing library function: {module['path']}:{name}")
        for name in set(module["values"]) - values:
            blockers.append(f"Missing library value: {module['path']}:{name}")
        for name, methods in module["classes"].items():
            if name not in classes:
                blockers.append(f"Missing library class: {module['path']}:{name}")
            for method in set(methods) - classes.get(name, set()):
                blockers.append(f"Missing library method: {module['path']}:{name}.{method}")
        result = subprocess.run([str(compiler), "check", str(path), "--stdlib", str(LIBRARY), "--json"],
                                capture_output=True, text=True, timeout=90)
        report["libraries"].append({"path": module["path"], "checked": result.returncode == 0})
        if result.returncode:
            blockers.append(f"Library has no successful native check: {module['path']}")

    with tempfile.TemporaryDirectory(prefix="as-a3-cutover-") as temporary:
        check_builtins(compiler, report, Path(temporary))
        for relative in baseline["examples"]:
            path = ROOT / relative
            if not path.is_file():
                blockers.append(f"Example disappeared: {relative}")
                continue
            result = subprocess.run([str(compiler), "build", str(path), "--stdlib", str(LIBRARY),
                                     "--emit-llvm", "-o", str(Path(temporary) / "example.ll"), "--json"],
                                    capture_output=True, text=True, timeout=90)
            report["examples"].append({"path": relative, "lowered": result.returncode == 0})
            if result.returncode:
                blockers.append(f"Example has no successful native lowering: {relative}")
    report["passed"] = not blockers
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = inventory(args.compiler.resolve())
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"A3-only cutover: {len(report['blockers'])} unresolved requirements; {args.report}")
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
