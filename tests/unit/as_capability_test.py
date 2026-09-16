#!/usr/bin/env python3
"""Native capability values, attenuation, GC and actual kernel-grant inputs."""

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/capability/main.as"
RUNTIME_FIXTURE = ROOT / "tests/unit/as_capability_runtime_test.c"
EXPECTED = "native capabilities ok\n"


def require_output(result):
    assert result.returncode == 0 and result.stdout == EXPECTED, result


def kernel_grants(work):
    """Read each side of the capability translation from its real C header."""
    source = work / "capability-oracle.c"
    source.write_text('''#include <stdio.h>
#include "logit_abi.h"
#include "runtime/capability_bits.h"
int main(void)
{
    printf("none %u %u\\n", 0u, 0u);
    printf("fs %u %u\\n", CAP_FS, AS_CAP_FS_READ | AS_CAP_FS_WRITE);
    printf("net %u %u\\n", CAP_NET, AS_CAP_NET);
    printf("raw %u %u\\n", CAP_RAW, AS_CAP_RAW);
    printf("proc %u %u\\n", CAP_PROC, AS_CAP_PROC);
    printf("gui %u %u\\n", CAP_GUI, AS_CAP_GUI);
    return 0;
}
''')
    binary = work / "capability-oracle"
    result = invoke([os.environ.get("CC", "clang"), source, "-o", binary,
                     "-I", ROOT / "include/abi", "-I", ROOT / "c/apps/as"])
    assert result.returncode == 0, result.stderr
    result = invoke([binary])
    assert result.returncode == 0, result
    grants = {}
    for line in result.stdout.splitlines():
        name, kernel, language = line.split()
        # A path-scoped FS grant also checks the kernel's prefix transfer.
        prefix = "/usr/as" if name == "fs" else ""
        grants[name] = (int(kernel), int(language), prefix)
    assert len(grants) == 6, grants
    return grants


def exercise(compiler):
    cases = {
        "caps-arity": ("def main() -> None:\n    caps(1)\n", "AS3204"),
        "scope-type": ("def main() -> None:\n    caps().scope(1)\n", "AS3202"),
        "mask-type": ("def main() -> None:\n    caps().without(1.0)\n", "AS3202"),
        "bits-arity": ("def main() -> None:\n    caps().bits(1)\n", "AS3204"),
        "path-result": ("def main() -> str:\n    return caps().path()\n", "AS3202"),
        "forged": ("def main() -> Cap:\n    return 63\n", "AS3202"),
        "ordered": ("def less(a: Cap, b: Cap) -> bool:\n    return a < b\n", "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-capability-") as temporary:
        work = Path(temporary)
        copy_runtime(work)
        runtime_test = work / "runtime-test.c"
        runtime_test.write_text(RUNTIME_FIXTURE.read_text())
        result = sanitized(runtime_test, work, work / "runtime-test", "-O2")
        assert result.returncode == 0, result
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        ir = work / "capability.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "capability", optimization))
        kernel_grants(work)
    print(f"PASS native capabilities: {len(cases)} diagnostics, attenuation, "
          "canonical paths, Optional text, Any, closure and O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-capability-control-") as temporary:
        work = Path(temporary)
        copy_runtime(work)
        heap = work / "heap.c"
        anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
        text = heap.read_text()
        assert text.count(anchor) == 1
        heap.write_text(text.replace(anchor, anchor + "\n    at_gc_collect();"))
        ir = work / "capability.ll"
        emit_ir(compiler, FIXTURE, ir)
        require_output(sanitized(ir, work, work / "positive", "-O0"))

        capability = work / "capability.c"
        original = capability.read_text()
        runtime_test = work / "runtime-test.c"
        runtime_test.write_text(RUNTIME_FIXTURE.read_text())
        result = sanitized(runtime_test, work, work / "runtime-positive", "-O0")
        assert result.returncode == 0, result

        anchor = "held_bits = 0;"
        assert original.count(anchor) == 1
        capability.write_text(original.replace(anchor, "held_bits = bits;"))
        result = sanitized(runtime_test, work, work / "lost-prefix", "-O0")
        assert result.returncode == 4, result
        capability.write_text(original)

        anchor = "if (parent->length > 1 &&"
        assert original.count(anchor) == 1
        capability.write_text(original.replace(anchor, "if (0 && parent->length > 1 &&"))
        result = sanitized(ir, work, work / "widening", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        capability.write_text(original)

        # Remove only Cap scanners, keeping all other roots. The program must
        # fail because its capability dies, not because a different object did.
        text = ir.read_text()
        scans = re.findall(r'define internal void @(scan\d+)\(ptr %slot\) \{\nentry:\n'
                           r'  %pointer = load ptr, ptr %slot\n'
                           r'  call void @at_gc_mark\(ptr %pointer\)\n', text)
        # Cap is the primitive used by at_caps_value/at_cap_scope result roots.
        call = re.search(r'(%v\d+) = call ptr @at_caps_value\(\)\n', text)
        assert call
        root_store = re.search(r'store ptr ' + re.escape(call[1]) + r', ptr (%rootvalue\d+)', text)
        assert root_store
        scan = re.search(r'call void @at_gc_root\(ptr %root\d+, ptr '
                         + re.escape(root_store[1]) + r', ptr @(scan\d+)\)', text)
        assert scan and scan[1] in scans, "Cap root mutation anchor changed"
        head = f"define internal void @{scan[1]}(ptr %slot) {{\nentry:\n"
        begin = text.index(head)
        end = text.index("}\n", begin)
        mutated = text[:begin] + head + "  ret void\n" + text[end:]
        ir.write_text(mutated)
        result = sanitized(ir, work, work / "missing-cap-root", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS capability controls: lost-prefix grant and widening fail; missing Cap roots fail under ASan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
