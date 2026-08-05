#!/usr/bin/env python3
"""Generate compile_commands.json so the IDE (clangd / C-C++ IntelliSense) analyzes
the tree exactly the way the Makefile builds it: x86_64-elf freestanding target with
every project include dir on the path. Without this the IDE falls back to the host
arm64/macOS-SDK target and reports spurious 'stdio.h not found' / '__sFILE has no
field' errors for our own freestanding headers.

Can be run from any directory:  python3 tools/gen_compile_commands.py
Paths are anchored at the repo root (this script's parent dir), and the source
walk uses os.walk -- no dependency on cwd or a host GNU find. The output is
machine-specific (absolute directory) and is .gitignored.
"""
import json, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def _rel(p):
    return os.path.relpath(p, ROOT).replace(os.sep, "/")

def find_dirs(*bases):
    out = []
    for base in bases:
        for dirpath, _dirnames, _files in os.walk(os.path.join(ROOT, base)):
            out.append(_rel(dirpath))
    return out

def find_files(*bases, suffix):
    out = []
    for base in bases:
        for dirpath, _dirnames, files in os.walk(os.path.join(ROOT, base)):
            for f in files:
                if f.endswith(suffix):
                    out.append(_rel(os.path.join(dirpath, f)))
    return out

incdirs = find_dirs("c", "include")
INC = [f"-I{d}" for d in incdirs]

JS_INC  = ["-Ithird_party/libm", "-Ithird_party/quickjs"]
CSS     = "third_party/css"
CSS_INC = [f"-I{CSS}/libwapcaplet/include", f"-I{CSS}/libparserutils/include",
           f"-I{CSS}/libcss/include", f"-I{CSS}/libcss/src",
           f"-I{CSS}/libparserutils/src"]

# Common freestanding flags (mirror CFLAGS/UCFLAGS; -nostdlibinc keeps clang's own
# builtin headers but blocks the macOS SDK so <stdio.h> resolves to ours).
BASE = ["clang", "--target=x86_64-elf", "-ffreestanding", "-nostdlib", "-nostdlibinc",
        "-fno-stack-protector", "-fno-pic", "-fno-pie",
        "-mno-red-zone", "-mno-mmx", "-msse", "-msse2", "-std=c11"]

# The engine bundle (QuickJS + libm + mini-libc + browser) is built with JS_CF.
ENGINE = ["-include", "features.h", '-DCONFIG_VERSION="aether-2024"',
          "-DAETHER_OS", "-DCONFIG_STACK_CHECK"] + JS_INC

def flags_for(path):
    if path.startswith("third_party/css/"):
        return BASE + ["-fcommon", "-D_ALIGNED=", "-DWITHOUT_ICONV_FILTER"] + CSS_INC + INC
    if (path.startswith("third_party/quickjs/") or path.startswith("third_party/libm/")
            or path.startswith("c/apps/libc/") or path.startswith("c/apps/browser/")
            or path.startswith("c/apps/js")):
        return BASE + ENGINE + INC
    return BASE + INC

files = find_files("c", "third_party", suffix=".c")
db = [{"directory": ROOT, "file": f, "arguments": flags_for(f) + ["-c", f]} for f in files]

with open(os.path.join(ROOT, "compile_commands.json"), "w") as fh:
    json.dump(db, fh, indent=1)
print(f"wrote compile_commands.json: {len(db)} entries")
