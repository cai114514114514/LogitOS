#!/usr/bin/env python3
"""Behavioral checks for the initial typed/native compiler path.

The tests use the real CLI and LLVM compiler, never a substitute interpreter.
O0 and O2 must agree on normal output and failure behavior. This is a host
gate; it does not establish LogitOS guest support or full language coverage.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, timeout=30, **kwargs)


def check_source(compiler, source):
    result = run([str(compiler), "check", "--json", "--stdin", "example.as"], input=source)
    report = json.loads(result.stdout)
    assert result.returncode == (0 if report["ok"] else 1), (result, report)
    return report


def exercise(compiler, directory):
    checks = 0

    def require(name, condition):
        nonlocal checks
        if not condition:
            raise AssertionError(name)
        checks += 1

    flow = """# aether: 3
def choose(flag: bool) -> i64:
    if flag:
        selected = 7
    else:
        selected = 9
    return selected

def main() -> i64:
    print(choose(True), choose(False))
    return 0
"""
    missing_assignment = flow.replace("        selected = 9", "        pass")
    report = check_source(compiler, missing_assignment)
    require("branch-initialization", any(d["code"] == "AS3206" for d in report["diagnostics"]))

    loop = """# aether: 3
def main() -> i64:
    for i in range(0):
        value = 4
    return value
"""
    report = check_source(compiler, loop)
    require("zero-iteration-loop", any(d["code"] == "AS3206" for d in report["diagnostics"]))

    wrong_type = """# aether: 3
def main() -> i64:
    text = "中文"
    value: i64 = text
    return 0
"""
    report = check_source(compiler, wrong_type)
    errors = [d for d in report["diagnostics"] if d["code"] == "AS3202"]
    require("type-error-location", len(errors) == 1 and errors[0]["line"] == 4)
    require("source-byte-identity", report["source_bytes"] == len(wrong_type.encode()))

    invalid_programs = {
        "readonly-nested-field": ("""struct Point:
    x: i64
def modify(points: Slice[Point]) -> None:
    points[0].x = 7
""", "AS3400"),
        "borrow-hidden-in-return": ("""struct View:
    data: Slice[i64]
def escape(data: Slice[i64]) -> View:
    return View(data)
""", "AS3400"),
        "assert-requires-bool": ("""def main() -> None:
    assert 1
""", "AS3202"),
        "float-shift": ("""def main() -> None:
    print(1.0 << 2.0)
""", "AS3202"),
        "return-after-break-is-unreachable": ("""def missing() -> i64:
    for i in range(2):
        break
        return 1
""", "AS3208"),
        "private-conflicting-calls": ("""def _identity(value):
    return value
def main() -> None:
    print(_identity(1), _identity(2.0))
""", "AS3202"),
        "recursive-inference-needs-annotation": ("""def _loop(value):
    return _loop(value)
def main() -> None:
    _loop(1)
""", "AS3201"),
        "private-missing-return": ("""def _maybe(flag: bool):
    if flag:
        return 2
def main() -> None:
    _maybe(False)
""", "AS3208"),
        "void-is-not-a-local-value": ("""def main() -> None:
    value = print(1)
""", "AS3202"),
    }
    for name, (body, code) in invalid_programs.items():
        report = check_source(compiler, "# aether: 3\n" + body)
        require(name, any(d["code"] == code for d in report["diagnostics"]))

    # Code Studio's first program was `#aether: 3` plus a print, often indented
    # under the cookie. That used to look like unversioned A2 after the VM left.
    compact = check_source(compiler, '#aether: 3\ndef main() -> None:\n    pass\n')
    require("compact-version-cookie", compact["ok"])
    script = check_source(compiler, '#aether: 3\nprint("hello world")\n')
    require("top-level-print-is-main", script["ok"])
    indented = check_source(compiler, '#aether: 3\n    print("hello world")\n')
    require("indented-script-under-cookie", indented["ok"])
    mixed = check_source(
        compiler, '# aether: 3.0\nprint(1)\ndef main() -> None:\n    print(2)\n')
    require("script-and-main-conflict", not mixed["ok"])

    cases = [
        ("flow", flow, 0, "7 9\n"),
        ("overflow", """# aether: 3
def fail() -> i64:
    value: i8 = 127
    value += 1
    return i64(value)
def main() -> i64:
    answer = fail()
    print("UNREACHABLE", answer)
    return 0
""", 1, "OverflowError"),
        ("bounds", """# aether: 3
def main() -> i64:
    values: Array[i64, 2] = [1, 2]
    print(values[3])
    return 0
""", 1, "IndexError"),
        ("studio-hello", '#aether: 3\nprint("hello world")\n', 0, "hello world\n"),
        ("studio-hello-indented", '#aether: 3\n    print("hello world")\n', 0, "hello world\n"),
    ]
    cases += [
        ("private-inference", """# aether: 3
def _double(value):
    return value * 2

def _seed():
    return 5

def _factorial(value: i64) -> i64:
    if value == 0:
        return 1
    return value * _factorial(value - 1)

def main() -> None:
    print(_double(_seed()), _factorial(5))
""", 0, "10 120\n"),
        ("loop-temporary-storage", """# aether: 3
def pair(value: i64) -> Array[i64, 2]:
    return [value, value + 1]
def sum_pair(values: Slice[i64]) -> i64:
    return values[0] + values[1]
def main() -> None:
    total = 0
    for i in range(100000):
        for j in range(2):
            total += sum_pair(pair(j))
    assert total == 400000
    print(total)
""", 0, "400000\n"),
        ("range-step", """# aether: 3
def main() -> None:
    total = 0
    for i in range(5, -1, -2):
        total += i
    assert total == 9
    count = 0
    for i in range(9223372036854775806, 9223372036854775807, 2):
        count += 1
    for i in range(-9223372036854775807, -9223372036854775808, -2):
        count += 1
    for i in range(1, 10, -1):
        assert False
    for i in range(10, 1, 1):
        assert False
    print(total, count)
""", 0, "9 2\n"),
        ("zero-step", """# aether: 3
def main() -> None:
    for i in range(0, 0, 0):
        print("UNREACHABLE")
""", 1, "ValueError"),
        ("continue-flow", """# aether: 3
def main() -> i64:
    for i in range(3):
        if i == 0:
            continue
        else:
            value = i * 2
        print(value)
    return 0
""", 0, "2\n4\n"),
        ("literal-radix", """# aether: 3
def main() -> None:
    print(08, 010, 0xff, -010)
""", 0, "8 10 255 -10\n"),
        ("power-and-shift", """# aether: 3
def main() -> None:
    base: i8 = 100
    assert base ** 1 == base
    assert 0 ** 0 == 1
    assert 2 ** 3 ** 2 == 512
    assert (-2) ** 3 == -8
    assert (-1) ** 9223372036854775807 == -1
    assert (-64) << 1 == -128
    assert (-7) >> 1 == -4
    assert 0 << 63 == 0
    top: u64 = 9223372036854775808
    assert top >> 63 == 1
    print("numeric rules hold")
""", 0, "numeric rules hold\n"),
    ]
    # Check every machine width and both signednesses. O0/O2 execution below
    # ensures the overflow result is not an accident of LLVM optimization.
    for signed in (True, False):
        for bits in (8, 16, 32, 64):
            typename = ("i" if signed else "u") + str(bits)
            maximum = (1 << (bits - int(signed))) - 1
            body = f"""# aether: 3
def main() -> None:
    value: {typename} = {maximum}
    print(value << 1)
    print("UNREACHABLE")
"""
            cases.append((f"shift-overflow-{typename}", body, 1, "OverflowError"))

    for name, expression, error in (
        ("power-overflow", "2 ** 63", "OverflowError"),
        ("power-negative", "2 ** -1", "ValueError"),
        ("shift-negative", "1 << -1", "ValueError"),
        ("shift-width", "1 >> 64", "ValueError"),
    ):
        body = f"# aether: 3\ndef main() -> None:\n    print({expression})\n    print(\"UNREACHABLE\")\n"
        cases.append((name, body, 1, error))
    case_arguments = {}
    for name in ("numeric", "modules", "binary"):
        fixture = ROOT / "tests/fixtures/astyped" / name / "main.as"
        expected = {"numeric": "中文 native 30 4\n", "modules": "30\n", "binary": "header 3 4096\n"}[name]
        cases.append((name, fixture, 0, expected))
        if name == "binary":
            # The binary project now reads a real file instead of a constant
            # array. Keep this base gate and supply the shared input artifact.
            from as_binary_test import binary_samples
            sample = directory / "binary-input.bin"
            sample.write_bytes(binary_samples()["valid"][0])
            case_arguments[name] = [str(sample)]

    nested_type = "i64"
    nested_value = "7"
    for _ in range(24):
        nested_type = f"Array[{nested_type}, 1]"
        nested_value = f"[{nested_value}]"
    nested = f"# aether: 3\ndef main() -> None:\n    value: {nested_type} = {nested_value}\n    print(value{'[0]' * 24})\n"
    cases.append(("deep-array-layout", nested, 0, "7\n"))

    clang = os.environ.get("AETHER_CLANG", "clang")
    from as_runtime import runtime_sources
    runtime = runtime_sources()
    for name, source, expected_status, expected_output in cases:
        if isinstance(source, Path):
            source_path = source
        else:
            source_path = directory / f"{name}.as"
            source_path.write_text(source)
        ir_path = directory / f"{name}.ll"
        built = run([str(compiler), "build", str(source_path), "--emit-llvm", "-o", str(ir_path)])
        require(f"{name}-lowering: {built.stdout} {built.stderr}", built.returncode == 0)

        for optimization in ("-O0", "-O2"):
            binary = directory / f"{name}{optimization}"
            linked = run([clang, optimization, str(ir_path), *map(str, runtime), "-o", str(binary)])
            require(f"{name}-{optimization}-llvm-validation: {linked.stderr}", linked.returncode == 0)
            executed = run([str(binary), *case_arguments.get(name, [])])
            require(f"{name}-{optimization}-status", executed.returncode == expected_status)
            if expected_status:
                require(f"{name}-{optimization}-error", expected_output in executed.stderr)
                require(f"{name}-{optimization}-propagation", "UNREACHABLE" not in executed.stdout)
            else:
                require(f"{name}-{optimization}-output", executed.stdout == expected_output)

    # Exercise the driver as well as emitted IR. An always-successful `as test`
    # would otherwise satisfy every positive compiler-only assertion above.
    suite = directory / "suite.as"
    suite.write_text("""# aether: 3
def test_arithmetic() -> None:
    assert 3 ** 3 == 27
def test_failure() -> None:
    assert False
def test_late() -> None:
    print("UNREACHABLE")
""")
    toolchain = str(ROOT / "c/apps/as/runtime")
    for debug in (False, True):
        command = [str(compiler), "test", str(suite), "--json", "--toolchain", toolchain]
        if debug:
            command.append("--debug")
        executed = run(command)
        result = json.loads(executed.stdout)
        require("test-assertion-exit", executed.returncode == 1 and result["exit_code"] == 1)
        require("test-assertion-location", "suite.as:5:5: AssertionError" in result["output"])
        require("test-stops-after-failure", "UNREACHABLE" not in result["output"])
        require("test-snapshot", result["snapshot"]["source_bytes"] == len(suite.read_bytes()))

    suite.write_text(suite.read_text().replace("assert False", "assert True"))
    passed = run([str(compiler), "test", str(suite), "--json", "--toolchain", toolchain])
    require("test-success-exit", passed.returncode == 0 and json.loads(passed.stdout)["ok"])

    # Check complete import-cycle paths rather than just the closing edge.
    cycle = directory / "cycle"
    cycle.mkdir()
    for name, dependency in (("first", "second"), ("second", "third"), ("third", "first")):
        (cycle / f"{name}.as").write_text(f"# aether: 3\nimport {dependency}\n")
    checked = run([str(compiler), "check", str(cycle / "first.as"), "--json"])
    report = json.loads(checked.stdout)
    diagnostic = next(d for d in report["diagnostics"] if d["code"] == "AS3301")
    require("complete-import-cycle", [Path(p["path"]).stem for p in diagnostic["related"]] ==
            ["first", "second", "third", "first"])

    # Nominal structs are scoped by their declaring module, not just spelling.
    # Both dependencies intentionally export a distinct type named Point.
    modules = directory / "type-scopes"
    modules.mkdir()
    for name, typename, value in (("integer", "i64", "7"), ("floating", "f64", "2.5")):
        (modules / f"{name}.as").write_text(f"# aether: 3\nstruct Point:\n    x: {typename}\ndef make() -> Point:\n    return Point({value})\n")
    module_entry = modules / "main.as"
    module_entry.write_text("# aether: 3\nimport integer\nimport floating\ndef main() -> None:\n    print(integer.make().x, floating.make().x)\n")
    executed = run([str(compiler), "run", str(module_entry), "--json", "--toolchain", toolchain])
    require("same-type-name-in-distinct-modules", executed.returncode == 0 and
            json.loads(executed.stdout)["output"] == "7 2.5\n")
    module_entry.write_text("# aether: 3\nfrom integer import missing\ndef main() -> None:\n    pass\n")
    report = json.loads(run([str(compiler), "check", str(module_entry), "--json"]).stdout)
    require("unused-import-still-checked", any(d["code"] == "AS3300" for d in report["diagnostics"]))
    module_entry.write_text("# aether: 3\nimport integer\ndef main() -> None:\n    print(Point(1))\n")
    report = json.loads(run([str(compiler), "check", str(module_entry), "--json"]).stdout)
    require("loading-module-does-not-import-types", not report["ok"])

    entry = directory / "entry.as"
    entry.write_text("# aether: 3\nimport dependency\ndef main() -> None:\n    print(dependency.answer())\n")
    dependency = directory / "dependency.as"
    dependency.write_text("# aether: 3\ndef answer() -> i64:\n    return 1\n")
    overlay = directory / "overlay.txt"
    overlay.write_text(dependency.read_text().replace("return 1", "return 42"))
    executed = run([str(compiler), "run", str(entry), "--json", "--toolchain", toolchain,
                    "--overlay", str(dependency), str(overlay)])
    result = json.loads(executed.stdout)
    require("dependency-overlay-is-executed", executed.returncode == 0 and result["output"] == "42\n")
    require("dependency-overlay-preserves-disk", "return 1" in dependency.read_text())

    for invalid in (b"# aether: 3\n\0", b"# aether: 3\n" + b" " * (1024 * 1024)):
        overlay.write_bytes(invalid)
        checked = run([str(compiler), "check", str(entry), "--json", "--overlay", str(dependency), str(overlay)])
        require("invalid-overlay-is-not-truncated-and-accepted", checked.returncode == 2)

    # Publishing a failed link must preserve the last good executable, whose
    # exact bytes stand in for a user's existing artifact here.
    artifact = directory / "saved-program"
    artifact.write_bytes(b"previous successful build")
    environment = dict(os.environ, AETHER_CLANG="/nonexistent/aether-test-clang")
    failed = run([str(compiler), "build", str(entry), "--json", "-o", str(artifact),
                  "--toolchain", toolchain], env=environment)
    require("link-failure-preserves-artifact", failed.returncode != 0 and
            artifact.read_bytes() == b"previous successful build")
    built = run([str(compiler), "build", str(entry), "--json", "-o", str(artifact),
                 "--toolchain", toolchain])
    require("artifact-atomic-publication", built.returncode == 0 and run([str(artifact)]).stdout == "1\n")

    invalid_entry = directory / "no-main.as"
    invalid_entry.write_text("# aether: 3\ndef helper() -> None:\n    pass\n")
    llvm_output = directory / "previous.ll"
    llvm_output.write_text("previous LLVM artifact\n")
    failed = run([str(compiler), "build", str(invalid_entry), "--emit-llvm", "-o", str(llvm_output)])
    require("lowering-failure-preserves-llvm-artifact", failed.returncode != 0 and
            llvm_output.read_text() == "previous LLVM artifact\n")

    print(f"PASS {checks} typed/native host checks")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-typed-test-") as directory:
        exercise(args.compiler.resolve(), Path(directory))


if __name__ == "__main__":
    main()
