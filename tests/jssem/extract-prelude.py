#!/usr/bin/env python3
"""Extract a JS prelude that lives as a C string literal in the browser.

WHY THIS EXISTS. /bin/jssem links $(ENGINE_OBJ) -- the browser's ENGINE -- but
not the browser's GLOBALS. Intl, structuredClone and queueMicrotask are JS
preludes in c/apps/browser/{js_intl,js_platform}.c, so a bare-engine transcript
that says "Intl is not defined" is a true fact about the engine and a FALSE
fact about the browser. This lifts the prelude out of the C source verbatim so
the same bytes the browser evaluates can be evaluated by the probe and by node.

Verbatim matters: retyping the polyfill would measure a copy of it.

    extract-prelude.py <file.c> <C-identifier>   > prelude.js
"""
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
ident = sys.argv[2]

m = re.search(r"\b" + re.escape(ident) + r"\s*=\s*", src)
if not m:
    sys.exit("no definition of %s in %s" % (ident, sys.argv[1]))

i = m.end()
out = []
n = len(src)
while i < n:
    c = src[i]
    if c == '"':
        j = i + 1
        buf = []
        while j < n:
            if src[j] == "\\":
                buf.append(src[j:j + 2]); j += 2; continue
            if src[j] == '"':
                break
            buf.append(src[j]); j += 1
        out.append("".join(buf))
        i = j + 1
        continue
    if c == ";":
        break
    if c == "/" and i + 1 < n and src[i + 1] == "*":
        i = src.index("*/", i) + 2
        continue
    if c == "/" and i + 1 < n and src[i + 1] == "/":
        i = src.index("\n", i) + 1
        continue
    i += 1

# C escapes that survive into the JS text. Only the ones a prelude uses.
s = "".join(out)
s = s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\")
sys.stdout.write(s)
