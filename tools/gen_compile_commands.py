#!/usr/bin/env python3
"""Generate compile_commands.json so the IDE (clangd / C-C++ IntelliSense) analyzes
the tree exactly the way the Makefile builds it: x86_64-elf freestanding target with
every project include dir on the path. Without this the IDE falls back to the host
arm64/macOS-SDK target and reports spurious 'stdio.h not found' / '__sFILE has no
field' errors for our own freestanding headers.

Run from the repo root:  python3 tools/gen_compile_commands.py
The output is machine-specific (absolute directory) and is .gitignored.
"""
import json, os, subprocess

ROOT = os.getcwd()

def find(*args):
    return subprocess.check_output(["find", *args]).decode().split()

incdirs = find("src", "include", "-type", "d")
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
            or path.startswith("src/apps/libc/") or path.startswith("src/apps/browser/")
            or path.startswith("src/apps/js")):
        return BASE + ENGINE + INC
    return BASE + INC

files = find("src", "third_party", "-name", "*.c")
db = [{"directory": ROOT, "file": f, "arguments": flags_for(f) + ["-c", f]} for f in files]

with open("compile_commands.json", "w") as fh:
    json.dump(db, fh, indent=1)
print(f"wrote compile_commands.json: {len(db)} entries")
