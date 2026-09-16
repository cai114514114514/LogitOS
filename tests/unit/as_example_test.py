#!/usr/bin/env python3
"""Execute rewritten examples natively against their frozen pre-migration output.

This manifest is also consumed by the guest runner and the retiring VM gate.
An example moves out of the VM gate only when it has a native execution oracle.
The fixtures remain the actual shipped sources, not simplified test copies.
"""

import argparse
from pathlib import Path
import tempfile

from as_managed_test import emit_ir, sanitized, RUNTIME, ROOT

EXAMPLES = ROOT / "fsroot/as/examples"
DEMO_SOURCE = ROOT / "fsroot/docs/demo.as"
DEMO_OUTPUT = "sum of squares: 30\nbits: 15 16 1024\n"
PORT_OUTPUT = (
    "PORTS: start\nports lines: true\nports pipe: alpha\n"
    "ports redir: redirected\nports scopes: 64 true\nports borrow: true\nports ok\n"
)

# Based on original outputs captured before rewriting (display changes below).
# Keep full stdout:
# a trailing "ok" marker would miss a broken operation earlier in the example.
OUTPUTS = {
    "hello": "Hello from AetherScript!\ncount 0\ncount 1\ncount 2\ncount 3\ncount 4\n",
    "fib": "fib(20) = 6765\n",
    "ptr": "p[0] + p[1] = 1337\n",
    # A3 exposes a typed overflow exception; f64 printing uses 17 significant
    # digits. Neither change alters the checked value or the executed operation.
    "checked_numeric": (
        "-9223372036854775808\nOverflowError\n-9223372036854775808\n"
        "-9223372036854775808\n9223372036854775807\n"
    ),
    "use_mod": "PI = 3.1415926535897931\nquad(2) = 16\nfrom-import square(9) = 81\n",
    # Set iteration order is not an API contract. The migrated teaching example
    # explicitly sorts before printing; every original operation/assert remains.
    "stdlib": (
        "squares: [1, 4, 9, 16]\nevens: [0, 2, 4, 6, 8]\nsum 1..10: 55\n"
        "sorted: [1, 1, 2, 3, 4, 5, 6, 9]\ngcd/lcm: 12 12\nisqrt 144: 12\n"
        "merged: {'a': 1, 'b': 2}\nchunks: [[1, 2], [3, 4], [5]]\nprime: 101\n"
        "words: ['logit', 'script', 'stdlib']\nset union: [1, 2, 3]\n"
        "stats: 2.5 2\nrandom: 7 a\npath: /usr/as/lib math.as .as\n"
        "bits: true 3 24\nstdlib ok\n"
    ),
    "dict": "len = 3\ntwo = 2\nhas three: true\nsum = 6\ncontains one: true\n",
    "strings": (
        "join: a,b,c\nsplit: ['a', 'b']\nstrip: [hi]\ncase: MIXED mixed\n"
        "replace: a--a--a\nfind: 2 -1\nfstr: n=7 sq=49\ntern: odd\n"
        "comp: [0, 1, 4, 9]\ncompif: 0,2,4,6\nswap: 2 1 unpack: 60\nstrings ok\n"
    ),
    "exc": "caught: boom\nruntime caught\nexc ok\n",
    "gc": "freed > 0: true\nbounded: true\ngc ok\n",
    "closure": "counts: 1 2 3\ndouble 21: 42\nadd10 to 5: 15\n",
    "classes": (
        "animal: Generic makes a sound\n"
        "dog: Rex makes a sound (woof) name: Rex\n"
        "counter: 1 2 3\n"
    ),
}


# Native LogitOS examples have genuine guest effects but intentionally refuse
# to interpret OS-specific syscall numbers on the host. Keep their full-output
# oracle alongside the portable corpus, consumed by the same guest runner.
GUEST_OUTPUTS = {
    **OUTPUTS,
    # Clock readings vary: as_clock_test validates the full measurement against
    # the independent RTC. None selects that oracle; it never skips validation.
    "monotonic": None,
    "ports": PORT_OUTPUT,
    "sys": "hello via syscall\n",
    "storchild": "STORCHILD readback 8 bytes-ok 1\n",
    "sysdemo": (
        "roundtrip: written by AetherScript\nls has it: true\n"
        "spawned-from-script\nspawn exit: 0\nclock sane: true\n"
        "pid=true cwd=/\nsysdemo ok\n"
    ),
}


def require_output(name, result):
    assert result.returncode == 0 and result.stdout == OUTPUTS[name], (name, result)


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-native-examples-") as temporary:
        work = Path(temporary)
        for name in OUTPUTS:
            source = EXAMPLES / (name + ".as")
            assert source.read_text().splitlines()[0] == "# aether: 3.0", source
            ir = work / (name + ".ll")
            emit_ir(compiler, source, ir)
            for optimization in ("-O0", "-O2"):
                result = sanitized(ir, RUNTIME, work / name, optimization)
                require_output(name, result)
        emit_ir(compiler, EXAMPLES / "ports.as", work / "ports.ll")
        for optimization in ("-O0", "-O2"):
            result = sanitized(work / "ports.ll", RUNTIME, work / "ports", optimization,
                               (EXAMPLES / "hello.as", work / "ports-output.txt"))
            assert result.returncode == 0 and result.stdout == PORT_OUTPUT, result
        emit_ir(compiler, DEMO_SOURCE, work / "demo.ll")
        for optimization in ("-O0", "-O2"):
            result = sanitized(work / "demo.ll", RUNTIME, work / "demo", optimization)
            assert result.returncode == 0 and result.stdout == DEMO_OUTPUT, result
    print(f"PASS native examples: {len(OUTPUTS) + 2} shipped sources, exact outputs, O0/O2 ASan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-example-control-") as temporary:
        work = Path(temporary)
        ir = work / "strings.ll"
        emit_ir(compiler, EXAMPLES / "strings.as", ir)
        require_output("strings", sanitized(ir, RUNTIME, work / "positive", "-O0"))

        source = work / "strings.as"
        text = (EXAMPLES / "strings.as").read_text()
        assert text.count("a, b = b, a") == 1, "swap mutation anchor changed"
        source.write_text(text.replace("a, b = b, a", "a, b = a, b"))
        emit_ir(compiler, source, ir)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode == 0 and result.stdout.endswith("strings ok\n"), result
        try:
            require_output("strings", result)
        except AssertionError:
            assert "swap: 1 2 unpack: 60" in result.stdout, result
        else:
            raise AssertionError("Native example oracle accepted a broken swap")
    print("PASS example control: unchanged success marker cannot hide a broken swap")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
